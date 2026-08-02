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
#include "sdrplay_source.h"
#include "airspyhf_source.h"

// Android builds the USB/librtlsdr local-hardware path; iOS builds only the
// RTL-TCP path (no USB host SDR on iOS). The USB code stays compiled on iOS via a
// no-op rtl-sdr stub (start() is never invoked there), so the shared DSP/net/
// RTL-TCP code below needs no per-call-site #ifdefs.
#ifdef __ANDROID__
  #include <android/log.h>
  #include <rtl-sdr.h>
#elif defined(VIBE_HAVE_LIBRTLSDR)
  // Desktop (macOS/Linux) with real librtlsdr linked — the standalone VibeServer drives the
  // dongle itself over libusb, so it needs the genuine header, not the stub.
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
#include <system_error>
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
#include "opus_audio_encoder.h" // VibeServer compressed audio (Opus; VIBE_HAVE_OPUS-gated)
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
    // ★ "fmdx" WAS a mode here and is gone: it widened the channel filter, which measured 10 dB
    // WORSE for RDS SNR (see pipeline.cpp chHalf). Accepted as an alias for wfm so a client with
    // it saved in a preference still tunes rather than falling through to NFM.
    if (mode == "fmdx")           return {ModeParams::WFM, 250000, 200000, 2};
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

// (IMA-ADPCM removed — VibeServer compressed audio is now Opus; see opus_audio_encoder.h.
//  Uncompressed stays raw int16 PCM as the safe, universal fallback.)

// ── VibeServer server-side state (declared here so Impl members can see it) ──
// LAN bind flag + PIN secret + compatibility limits + audio-codec toggle. The
// public setter methods that write these live after the Impl struct.
static std::atomic<bool>   g_serveOnLan{false};
static std::mutex          g_vsMtx;
static std::string         g_vsSecret;                 // empty = no PIN (open)
// Operator-chosen listen port. 0 = scan 48000..48049 for the first free one (historic behaviour).
// Set explicitly, we use THAT port or fail loudly: silently drifting to another port breaks a
// router port-forward or a saved bookmark, and "clients can't connect but the server says it's
// running" is a horrible symptom to diagnose. (Lived it: a stale mock squatting 48000 sent us to
// 48001 without a word.)
static std::atomic<int>    g_vsPort{0};
static std::atomic<double> g_vsMaxBandwidth{0.0};      // <=0 = no cap
static std::atomic<double> g_vsMaxFftRate{0.0};        // <=0 = server default (20 fps)
// ★ The IDLE SAVER is a CLIENT feature — after a while without interaction the listener asks us to
// drop the spectrum rate, which saves this host real FFT work and real radio time. Normally the
// listener may switch it off. A server on solar and cellular in the middle of nowhere cannot afford
// that choice, so its owner can make the saving MANDATORY: published to clients, which then lock
// their toggle on and say who set it (the same courtesy as lockedRate).
static std::atomic<bool>   g_vsForceIdle{false};
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
// ★★ May a client have RAW audio if it does not ask for Opus?
//
// Uncompressed is 48 kHz x 2 ch x 2 B = ~187 KB/s PER LISTENER, ~20x the Opus
// stream, and it is the OWNER'S uplink it spends — the shack at the allotment on
// a flaky link is exactly who cannot afford a visitor demanding it. So the
// operator gets to refuse it, in the same spirit as the max rate/fps ceiling.
//
// ★ PERMANENT, and OFF BY DEFAULT — the same shape OWRX offers (Stuart's call).
// An operator control that other SDR servers already provide is a familiar
// affordance in a familiar place; keeping it costs nothing, and removing it
// later would surprise anyone who had come to rely on it.
//
// ★ DEFAULTS TO OFF. The compatibility worry turned out to be almost empty:
// VibeServer debuted in v8, so App Store 6.1 cannot see a VibeServer at all, and
// the ONLY client that opens /ws/audio without a `codec` parameter is TestFlight
// 9.0.1 — the build this release replaces. So the cost of defaulting off is one
// superseded TestFlight build, against every visitor otherwise silently spending
// ~187 KB/s of the owner's uplink.
//
// ★★ NOW THREE-WAY (VsUncompressedAudio), because there are TWO different reasons to
// want raw audio and they need different answers. The original reason was compatibility
// — a browser that cannot decode Opus should get sound rather than silence — and that
// wants NO user-facing control at all. The second reason is QUALITY: Opus at 64 kbps is
// audibly compressed on good headphones, which HansVanEijsden identified by ear within
// moments (2026-07-27), and that one is precisely a listener's choice to make.
// A single bool had to serve both and so served neither: switching it on to keep old
// browsers working also silently offered 187 KB/s to every visitor.
//   OFF(0)    never, for networked clients
//   CHOICE(1) listener may switch it on; a control appears in the audio menu
//   COMPAT(2) automatic fallback only, no control shown
static std::atomic<int>    g_vsUncompressedAudio{0};   // VsUncompressedAudio
// ★★★ THE ADMIN PASSWORD — a SECOND secret, and a different job from the PIN. The PIN gates
// ACCESS (may you listen at all); this gates CONTROL of the settings a stranger has no
// business touching on someone else's radio.
// ★ The two are independent on purpose: Hans's public receiver has NO pin (anyone may listen)
// but must still refuse a visitor switching the bias-T on.
// ★ Empty = no admin password set, and then nothing is protected — a host that has not asked
// for this should not find controls mysteriously refusing to work.
static std::mutex          g_vsAdminMtx;
static std::string         g_vsAdminSecret;
/** ★★ SESSION TIME LIMIT, minutes. 0 = unlimited (the default, and what every private
 *  receiver wants). Exists because one client per radio plus one radio per server means a
 *  PUBLIC VibeServer is a queue of one, and without a limit the first listener holds it all
 *  evening (Stuart, 2026-07-27).
 *  ★ LOOPBACK IS EXEMPT, always: the host listening on their own machine is not queueing for
 *  anything, and timing them out of their own radio would be absurd. Admin sessions are
 *  exempt too — the owner should not be able to lock themselves out. */
static std::atomic<int>     g_vsSessionLimitMin{0};
/** ★★★ THE COOLDOWN IS WHAT MAKES THE LIMIT REAL. Every client auto-reconnects on close, so a
 *  plain disconnect would be a blip: the same listener would retake the free radio within
 *  seconds and carry on, and the limit would be decorative. After a timeout the address is
 *  refused for this long, which is the window in which somebody else can actually get in.
 *  ★ Keyed on IP, chosen deliberately over the session id: the id is CLIENT-GENERATED, so a
 *  cooldown on it is advisory at best — clear it and you are back in. The cost is a household
 *  behind one router shares a cooldown, which is the accepted trade (Stuart, 2026-07-27). */
static constexpr int        kSessionCooldownSec = 120;

// Opus target bitrate (bits/sec) for compressed VibeServer audio — THE link-adaptive lever. 64 kbps
// is a near-transparent FM-stereo default; the client ramps it down over a constrained link.
static std::atomic<int>    g_vsOpusBitrate{64000};
// Client-requestable output bin count (waterfall width) — the FFT/BIN lever. Default = OUT_BINS
// (full res, e.g. the web client). A small screen on a constrained link (the watch over Bluetooth)
// asks for far fewer via /ws/user-spectrum?bins=N, cutting each SPEC frame from 22+OUT_BINS bytes to
// 22+N. Clamped [128, OUT_BINS]. Single-occupant, so one global suffices.
static std::atomic<int>    g_vsOutBins{OUT_BINS};

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

/** Is this peer the host itself? IPv4 loopback, IPv6 loopback, and the v4-mapped form. */
static bool isLoopback(const std::string& ip) {
    return ip.rfind("127.", 0) == 0 || ip == "::1" || ip == "::ffff:127.0.0.1";
}

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
    // ★ A THIRD SOURCE, following the shape the other two already set. The RSP is 14-bit
    // where the dongle is 8 — and 2026-07-26 established that RDS is limited by the FRONT
    // END, not by our decoder, so this is how that gets tested rather than argued about.
    std::unique_ptr<vibe::SdrplaySource> sdrp;
    std::unique_ptr<vibe::AirspyHfSource> ahf;   // Airspy HF+ (Discovery / Dual Port)
    int  sdrpIndex = 0;
    // ★★ One-shot AGC kick, once the stream is genuinely running. Cycling it inside open()
    // — immediately after Init, before any samples have flowed — still left it inert, so
    // the transition evidently has to happen against a LIVE stream rather than a device
    // that has merely been initialised. Stuart's suggestion, and it is the same sequence a
    // user performs by hand, just done for them a second in (2026-07-26).
    int  sdrpAgcKick = 0;
    // ★ What the USER wants, which the kick must respect. Somebody deliberately running
    // manual gain would otherwise have the AGC switched back on for them a second after
    // connecting — a fix for one person's problem becoming another's bug (Stuart).
    bool sdrpAgcWanted = true;
    bool sdrpSettling = true;      // true while the AGC is being kicked and settling
    bool useSdrplay() const { return (bool)sdrp; }
    bool useAirspyHf() const { return (bool)ahf; }
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
    /** ★★★ THE DEAD EDGES ARE NOT SPECTRUM — "FILL-IN BANDWIDTH", after SDR++ Brown.
     *  An HF+'s outer bins are the skirt of its own anti-alias filter: attenuated, increasingly
     *  aliased, and carrying nothing to receive. Left in, they do three kinds of harm — they look
     *  like a signal-free cliff at each end, they drag the auto-range statistics down (we already
     *  had to push EDGE_EXCLUDE_FRAC to 9% to keep them out of the noise-floor percentile), and
     *  they invite the user to tune into a region where nothing can be heard.
     *  ★ Values are Brown's, from `narrowSamplerate()` in his airspyhf_source — he calls them
     *  "experimental", i.e. MEASURED, which is the only kind of figure available: no driver
     *  reports a usable bandwidth. At 912 kHz that is 80 kHz per side, leaving 752 of 912 (82.5%).
     *  ★★ Cheaper than his version, because of where we sit: SDR++ resamples the IQ to the reduced
     *  rate (an arbitrary 752/912 ratio). We send FFT BINS, so cropping is just not sampling those
     *  bins — no filter, no resampler, and the client's span shrinks to match.
     *  ★ Stuart, on the alternative of hiding them behind a raised noise floor: "even artificially
     *  raising the floor just shows a flat cutoff" — cosmetic, and the statistics stay poisoned. */
    double edgeCutoffHz() const {
        if (!ahf) return 0.0;                 // only the HF+ has lobes this wide
        if (sampleRate <= 192001.0) return 30000.0;
        if (sampleRate <= 256001.0) return 40000.0;
        if (sampleRate <= 384001.0) return 50000.0;
        if (sampleRate <= 768001.0) return 60000.0;
        // ★★ AT 912 kHz WE CROP TO 768, NOT TO BROWN'S 752. Three reasons, and they are Stuart's:
        //    • 768 kHz is a rate AIRSPY THEMSELVES ADVERTISE for this radio, so the number on the
        //      screen is one the manufacturer uses rather than one we invented;
        //    • cropping to 752 removes the roll-off completely and leaves a hard vertical cliff at
        //      each end, which reads as a fault. Stopping 8 kHz short leaves the SHOULDER visible —
        //      the display drops away at the edges the way an SDRplay's does, which is what a
        //      receiver honestly looks like;
        //    • it still kills all the dead space: 72 kHz per side instead of 80, and the last
        //      8 kHz is skirt you can see rolling off, not a flat floor of nothing.
        //    ★ Brown's 80 kHz is the right figure for "where is it unusable"; this is the right
        //    figure for "what should a person be shown". They are different questions.
        return (sampleRate - 768000.0) / 2.0;   // 72 kHz per side at 912
    }
    /** The part of the capture that is actually receivable. */
    double usableSpan() const { return std::max(sampleRate * 0.5, sampleRate - 2.0 * edgeCutoffHz()); }
    double displaySpan() const { return spyFftSpan > 0.0 ? spyFftSpan : usableSpan(); }

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
    /** ★★★ A HARDWARE MOVE THAT THE COOLDOWN POSTPONED, waiting to be applied. 0 = none.
     *  The cooldown below exists because a PLL relock on every pan message audibly breaks the
     *  audio — but it used to DROP the move rather than defer it, and nothing ever retried. */
    double pendingDongle = 0.0;
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
    // ── Dongle hot-plug ─────────────────────────────────────────────────────
    // `stopping` distinguishes OUR cancel from the device disappearing; `deviceLost` is the state
    // everything else reads. `usbIndex` is what we reopen, and `usbSerial` is what we reopen it BY,
    // because indices renumber when another dongle is unplugged.
    std::atomic<bool> stopping{false};
    // A DELIBERATE restart (setSampleRate cancels and relaunches the read) looks identical to an
    // unplug from inside the capture thread — which is exactly how the first version cried wolf
    // while the radio was working perfectly.
    std::atomic<bool> restarting{false};
    std::atomic<bool> deviceLost{false};
    /** Capture has stopped and we did not ask it to. */
    std::atomic<bool> captureDown{false};
    /** When the last IQ buffer arrived. ★ THE ONLY RELIABLE UNPLUG SIGNAL.
     *  `rtlsdr_read_async` does NOT return when the dongle is pulled — libusb simply stops
     *  completing transfers, so the capture thread sits there forever and any detection that waits
     *  for that call to return waits for ever too. Silence is the signal. */
    std::atomic<double> lastIqAt{0.0};
    // Consecutive in-place stream restarts since the last healthy stretch — drives the backoff
    // that stops a wedged API being hammered, and for the Airspy decides when to escalate from
    // restarting the stream to reopening the device. Watchdog thread only.
    // ★ Renamed from sdrpRestarts: the RSP is no longer the only source that recovers.
    int    srcRestarts = 0;
    double lastRestartAt = 0.0;
    int usbIndex = -1;
    std::string usbSerial;
    std::thread hotplugThread;
    std::atomic<bool> hotplugRun{false};

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
        // ★★★ THE VFO IS NOT A POINT — IT HAS A BANDWIDTH, AND THE EDGES ARE WHERE IT DIES.
        //     This margin used to protect the VFO's CENTRE only, so a WFM signal could sit within
        //     91 kHz of the capture edge while its own sidebands hung over it. At 912 kHz on an
        //     HF+ that put RDS (+57 kHz) and the stereo subcarrier (+38 kHz) into the roll-off:
        //     RDS dropped and stereo turned hissy on a LOCAL, full-strength station (Stuart,
        //     2026-08-02, unlocking the VFO and panning it off screen on Heart).
        //     ★ Arithmetic at 912 kHz: half-span 456, margin 91.2, so the old limit let the VFO
        //     reach 364.8 kHz off centre — but the HF+'s usable half-span is only ~376 kHz (its
        //     dead lobe is ~80 kHz per side, sourced from SDR++ Brown's Fill-In figures). A
        //     200 kHz-wide WFM channel there is half outside the usable band.
        //     ★★ NOT AN AIRSPY QUIRK. Every radio rolls off at the edge of its capture; the HF+
        //     just has unusually wide dead lobes so it shows up first. Stuart: "same would happen
        //     with the sdrplay too, I've seen it before." So the clearance is taken from the
        //     DEMODULATED BANDWIDTH, which is true of every source.
        //     ★ Cheap for narrow modes: SSB takes 1.35 kHz off the limit, NFM 6 kHz — panning on
        //     HF is unchanged. It is WFM's 200 kHz that needed saying out loud.
        // ★ usableSpan(), not sampleRate: on an HF+ the outer 80 kHz per side is filter skirt, so
        //   "inside the capture" has to mean inside the part that can actually be received.
        const double lim = std::max(sampleRate * 0.05,
                                    usableSpan() / 2.0 - viewDongleMargin() - rxBwHz * 0.5);
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
        else if (useSdrplay()) sdrp->setFrequency((double)hz);
        else if (useAirspyHf()) ahf->setFrequency((double)hz);
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
    int rdsBer = -1;                         // RDS block error rate %, -1 = unknown
    float rdsSig = -99.0f;                   // 57 kHz level vs pilot, dB (-99 = none)
    std::atomic<bool> rdsxOn{false};         // a client has the Advanced RDS decoder open
    // ★★ ADMIN UNLOCK, per connected client. Cleared whenever the spectrum client changes, so
    // an unlock cannot outlive the session that earned it and be inherited by the next visitor.
    std::atomic<bool> adminOk{false};
    // Extended RDS, refreshed by the engine; guarded by rdsMtx like the rest.
    int rdsPty = -1, rdsTp = -1, rdsTa = -1, rdsMs = -1, rdsDi = -1;
    // ★ The same five UNCONFIRMED, for the client's RAW view. Live, never sticky.
    int rdsPtyRaw = -1, rdsTpRaw = -1, rdsTaRaw = -1, rdsMsRaw = -1, rdsDiRaw = -1;
    int rdsCtMin = -1, rdsCtOff = 0, rdsGrpTotal = 0, rdsAfSeen = 0;
    std::vector<int> rdsGrp;
    std::string rdsRtpTitle, rdsRtpArtist, rdsLongPs, rdsPtyn;
    int rdsLang = 0, rdsPinDay = 0, rdsPinHour = -1, rdsPinMin = 0;
    float rdsPhase = -1.0f;                  // RDS-to-pilot phase, degrees (-1 = no lock)
    float rdsPhaseCoh = 0.0f;                // ...and how much to believe it, 0..1
    // ★ How fast that phase is TURNING, deg/s. Coherence only catches FAST rotation; a slow
    // one keeps coherence high while the angle walks all the way round. See vibedsp.h.
    float rdsPhaseDrift = 0.0f;
    float rdsPilotDev = 0.0f, rdsDev = 0.0f; // injection levels, kHz deviation
    std::vector<vibedsp::RdsDecoder::Eon> rdsEon;
    std::vector<vibedsp::RdsDecoder::Oda> rdsOda;
    std::vector<int> rdsAf;
    std::vector<float> rdsConst;
    std::vector<float> rdsMpx;             // MPX spectrum, dB per bin
    std::atomic<bool> stereoDetected{false};
    // The audio client asked for Opus (via /ws/audio?codec=opus) AND this build can encode it.
    // Default OFF = raw PCM, so a client that can't decode Opus (today's web client) is never sent
    // it. Jr's future VibeServer backend opts in. Single-occupant, so one flag suffices.
    std::atomic<bool> audioWantsOpus{false};
    // ★ MONO-REQUEST. A client on a MONO output route (the watch built-in speaker) asks for mono via
    // /ws/audio?channels=1. We then fold the demod's stereo to mono BEFORE Opus-encoding, so the full
    // 64 kbps lands in ONE fullband channel instead of splitting across two band-limited channels the
    // client would only downmix anyway — same bandwidth, clearly better on the speaker. Default off =
    // send the demod's native channel count (stereo when the WFM pilot is locked). Reuses the exact
    // channel-switch path the pilot lock/unlock already drives every day, so nothing new to break.
    std::atomic<bool> audioForceMono{false};
    // VibeServer compressed-audio encoder — Opus (replaces IMA-ADPCM). Opus handles L/R stereo
    // natively, so there is no mid/side split; one encoder covers mono and stereo and reconfigures
    // itself on a channel change. Guarded: only the macOS core links libopus today.
#ifdef VIBE_HAVE_OPUS
    vibe::OpusAudioEncoder opusEnc;
#endif
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
    // The single occupant's session id (empty = free). Guarded by clientMtx. A client's spectrum +
    // audio sockets share this id; a second client is refused while it is held. See acceptWs.
    std::string occupantSession;
    /** When the current occupant claimed the slot (seconds, monotonic). 0 = nobody. Guarded by
     *  clientMtx alongside occupantSession — one lock for one piece of state. */
    double occupantSince = 0;
    /** The occupant's address, so a timeout can put THAT address on cooldown. */
    std::string occupantAddr;
    /** Warnings already sent this session, so each fires once: bit 0 = 2 min, bit 1 = 30 s.
     *  ★ A limit that ends a session with no warning reads as a crash. */
    int occupantWarned = 0;
    /** address -> monotonic time the cooldown ends. Pruned lazily on lookup. */
    std::map<std::string, double> cooldownUntil;
    std::atomic<uint64_t> frameCounter{0};

    std::mutex sendMtx; // serialises all WS writes (both directions are split, sends here)
    double specAuditMs = 0.0; long long specAuditFrames = 0;   // see onSpectrum's rate audit

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

        // ── Spectrum rate audit (passive) ──────────────────────────────────
        // ★ 20 fps is requested and ~14 arrives, on BOTH clients. The request is correct
        // (the engine is set to 20*FFT_AVG) and the emit path has no throttle, so the loss
        // is between the FFT and the wire — this counts what ACTUALLY leaves here, which is
        // the one number nobody has measured. Speaks only when it deviates, once per 5 s.
        {
            const double tMs = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (specAuditMs == 0.0) specAuditMs = tMs;
            ++specAuditFrames;
            if (tMs - specAuditMs >= 5000.0) {
                const double got = specAuditFrames * 1000.0 / (tMs - specAuditMs);
                const double want = fftRate;
                if (want > 0 && std::fabs(got - want) > want * 0.1)
                    LOGI("SPEC RATE: emitting %.1f fps, asked %.1f (engine %.1f, %.0f%% of target)",
                         got, want, want * FFT_AVG, 100.0 * got / want);
                specAuditFrames = 0; specAuditMs = tMs;
            }
        }

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
            const int outBins = g_vsOutBins.load();
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
            // ★ The RSP's live gain state, once a second. The AGC moves the IF reduction on
            // its own, so a slider position is NOT the truth — and the total system gain is
            // the one figure that makes two independent controls readable.
            // ★ 5 Hz, not 1. The IF slider follows the AGC live, and a thumb that jumps once
            // a second reads as broken rather than as tracking. A ~90 byte message at 5 Hz is
            // nothing next to the spectrum.
            // ★ ~1 s after the stream starts, cycle the AGC once. Off, then on: the API only
            // starts the loop on a TRANSITION, and assignment before Init is not one.
            // ★ Settling window: the AGC kick plus a few frames either side, reported to the
            // client so the waterfall's bounce is EXPLAINED rather than looking like a fault.
            // ★ An unexplained transient reads as a defect; a labelled one reads as a radio
            // settling, and the difference is entirely in whether we said so (Stuart).
            if (useSdrplay()) sdrpSettling = (n < 40);
            if (useSdrplay() && sdrpAgcWanted && sdrpAgcKick < 2 && n > 10) {
                if (++sdrpAgcKick == 1) sdrp->setIfAgc(false);
                else                    sdrp->setIfAgc(true);
            }
            if (n % 2 == 0 && useSdrplay()) {
                char gb[160];
                snprintf(gb, sizeof gb,
                    "{\"type\":\"rspstat\",\"sysGain\":%.1f,\"lna\":%d,\"ifgr\":%d,\"overload\":%d,"
                    "\"settling\":%d}",
                    sdrp->systemGainDb(), sdrp->currentLnaState(), sdrp->currentIfGr(),
                    sdrp->overloaded() ? 1 : 0, sdrpSettling ? 1 : 0);
                sendText(sock, gb);
            }
            // ★ The Advanced RDS payload runs FASTER than the metadata — a constellation
            // updated once a second reads as a still image, and its whole value is watching
            // the cloud tighten or spread as you tune. ~5 Hz, and only while the decoder is
            // open, so an ordinary listener never pays for it.
            if (rdsxOn.load() && n % 2 == 0) sendRdsExt(sock);
            if (n % 2 == 0) enforceSessionLimit();
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

        const int outBins = g_vsOutBins.load();
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
    // ★ PI on its own, the moment the decoder confirms it. It used to be set ONLY
    // inside rdsPsCb, so the station's identity was hostage to its NAME assembling —
    // and the name is the fragile part (8 characters across 4 groups, any one of which
    // can be lost), while PI is 16 error-protected bits repeated ~11 times a second.
    // Weak stations therefore reported nothing at all when they were in fact telling
    // us exactly who they were (2026-07-26).
    static void rdsPiCb(void* ctx, uint16_t pi) {
        Impl* t = (Impl*)ctx; std::lock_guard<std::mutex> lk(t->rdsMtx);
        t->rdsPi = (int)pi;
    }
    static void rdsExtCb(void* ctx, const vibedsp::RxPipeline::Callbacks::RdsExt& x) {
        Impl* t = (Impl*)ctx;
        if (!t->rdsxOn.load()) return;
        std::lock_guard<std::mutex> lk(t->rdsMtx);
        t->rdsPty = x.pty; t->rdsTp = x.tp; t->rdsTa = x.ta; t->rdsMs = x.ms; t->rdsDi = x.di;
        t->rdsPtyRaw = x.ptyRaw; t->rdsTpRaw = x.tpRaw; t->rdsTaRaw = x.taRaw;
        t->rdsMsRaw = x.msRaw;   t->rdsDiRaw = x.diRaw;
        t->rdsCtMin = x.ctMinutes; t->rdsCtOff = x.ctOffsetHalfHours;
        t->rdsGrpTotal = x.groupTotal; t->rdsAfSeen = x.afSeen;
        t->rdsAf.assign(x.afKhz, x.afKhz + x.nAf);
        t->rdsGrp.assign(x.groupCounts, x.groupCounts + 32);
        t->rdsConst.assign(x.constXY, x.constXY + x.nPts * 2);
        if (x.mpx && x.nMpx > 0) t->rdsMpx.assign(x.mpx, x.mpx + x.nMpx);
        t->rdsRtpTitle = x.rtpTitle ? x.rtpTitle : "";
        t->rdsRtpArtist = x.rtpArtist ? x.rtpArtist : "";
        t->rdsLongPs = x.longPs ? x.longPs : "";
        t->rdsPtyn = x.ptyn ? x.ptyn : "";
        t->rdsLang = x.language;
        t->rdsPinDay = x.pinDay; t->rdsPinHour = x.pinHour; t->rdsPinMin = x.pinMinute;
        t->rdsEon.assign(x.eon, x.eon + x.nEon);
        t->rdsOda.assign(x.oda, x.oda + x.nOda);
        t->rdsPhase = x.pilotPhaseDeg;
        t->rdsPhaseCoh = x.pilotPhaseCoherence;
        t->rdsPhaseDrift = x.pilotPhaseDriftDegPerSec;
        t->rdsPilotDev = x.pilotDevKHz;
        t->rdsDev      = x.rdsDevKHz;
    }
    static void rdsSigCb(void* ctx, float relDb) {
        Impl* t = (Impl*)ctx; std::lock_guard<std::mutex> lk(t->rdsMtx);
        t->rdsSig = relDb;
    }
    static void rdsBerCb(void* ctx, int percent) {
        Impl* t = (Impl*)ctx; std::lock_guard<std::mutex> lk(t->rdsMtx);
        t->rdsBer = percent;
    }
    static void rdsEccCb(void* ctx, uint8_t ecc) {
        Impl* t = (Impl*)ctx; std::lock_guard<std::mutex> lk(t->rdsMtx);
        t->rdsEcc = ecc;
    }
    static void stereoCb(void* ctx, bool locked) { ((Impl*)ctx)->stereoDetected.store(locked); }

    // Frame + send one block of interleaved int16 PCM to the audio client.
    //
    // Two formats, both self-describing by the header's format byte [1]:
    //   [1]=0  RAW int16 PCM  — the uncompressed/loopback path AND the universal safe fallback.
    //   [1]=3  OPUS           — compressed VibeServer audio (~4x lighter than the old ADPCM at
    //                           better quality; bitrate is the link-adaptive lever). Native stereo,
    //                           no mid/side. Each WS frame carries ONE Opus packet.
    //
    // Header (both): [0]=channels(1|2), [1]=format, [2..5]=sampleRate u32 LE. PCM appends int16
    // samples; Opus appends one packet. ADPCM (old formats 1/2) is retired — every VibeServer
    // consumer (its own web page, VibeSDR, Jr) is ours, so there is no third party to keep it for.
    void sendAudioPcm(const std::shared_ptr<net::Socket>& sock, const int16_t* pcm, int count, int ch) {
        if (count <= 0) return;

        // Mono-request fold (see audioForceMono): stereo → mono BEFORE encoding. Covers both the Opus
        // and PCM paths and the encoder rebuilds itself on the channel change, same as a pilot flip.
        std::vector<int16_t> monoBuf;
        if (audioForceMono.load() && ch == 2) {
            monoBuf.resize((size_t)count);
            for (int i = 0; i < count; i++)
                monoBuf[i] = (int16_t)(((int)pcm[i*2] + (int)pcm[i*2+1]) / 2);
            pcm = monoBuf.data(); ch = 1;
        }

#ifdef VIBE_HAVE_OPUS
        // Opus only when THIS client opted in (see acceptWs). A client that can't decode it — the
        // current web client — is never sent it, so nothing breaks; it gets PCM below.
        if (audioWantsOpus.load()) {
            opusEnc.setBitrate(g_vsOpusBitrate.load());
            std::vector<std::vector<uint8_t>> packets;
            opusEnc.encode(pcm, count, ch, packets);   // buffers into 20 ms frames internally
            const uint32_t sr = (uint32_t)vibe::OpusAudioEncoder::kSampleRate;   // always 48 kHz
            for (auto& pkt : packets) {
                std::vector<uint8_t> frame; frame.reserve(6 + pkt.size());
                frame.push_back((uint8_t)ch); frame.push_back(3);               // [0]=ch, [1]=3 Opus
                frame.push_back((uint8_t)(sr & 0xff));         frame.push_back((uint8_t)((sr >> 8) & 0xff));
                frame.push_back((uint8_t)((sr >> 16) & 0xff)); frame.push_back((uint8_t)((sr >> 24) & 0xff));
                frame.insert(frame.end(), pkt.begin(), pkt.end());
                sendWs(sock, 0x2, frame.data(), frame.size());
                vsAudioBytes.fetch_add(frame.size(), std::memory_order_relaxed);
            }
            return;
        }
#endif
        // No libopus in this build, or the client did not opt in → raw PCM (the safe fallback).
        // header: [0]=channels, [1]=0 (raw), [2..5]=sampleRate u32 LE, int16 PCM
        std::vector<uint8_t> frame(6 + (size_t)count * ch * 2);
        frame[0] = (uint8_t)ch; frame[1] = 0;
        uint32_t sr = (uint32_t)AUDIO_SR; std::memcpy(&frame[2], &sr, 4);
        std::memcpy(frame.data() + 6, pcm, (size_t)count * ch * 2);
        sendWs(sock, 0x2, frame.data(), frame.size());
        vsAudioBytes.fetch_add(frame.size(), std::memory_order_relaxed);
    }

    // ── Audio fault audit (passive) ────────────────────────────────────────
    // ★ MEASURE FIRST. The FM "mute" has now been mis-diagnosed twice from theory, so this
    // reports what the audio ACTUALLY is at the point it leaves the DSP, and distinguishes the
    // three ways it can go quiet — they have completely different causes:
    //   • non-finite : a NaN/Inf has latched into recursive state (the stereo PLL and the FM DC
    //                  blocker are both IIR) -> silence forever until a restart;
    //   • railed     : clipped flat against the limit -> inaudible, but the DSP is fine;
    //   • silent     : genuinely near zero -> the signal or an upstream stage has stopped.
    // ★★ This must never PERTURB anything (a probe shipped as behaviour cost a working radio on
    // 08-01), so it only reads, and it only speaks when something is wrong — no log spam, and
    // nothing at all in normal listening.
    struct AudioAudit {
        long long n = 0, nonFinite = 0, railed = 0, quiet = 0;
        double tMs = 0.0;
        void note(float v) {
            ++n;
            if (!std::isfinite(v)) { ++nonFinite; return; }
            const float a = std::fabs(v);
            if (a > 0.995f)      ++railed;
            else if (a < 1e-4f)  ++quiet;
        }
        // Returns true and fills `out` once a second IF the second was faulty.
        bool tick(double nowMs, char* out, size_t cap, const char* stage) {
            if (tMs == 0.0) { tMs = nowMs; return false; }
            if (nowMs - tMs < 1000.0 || n == 0) return false;
            const double nf = 100.0 * (double)nonFinite / (double)n;
            const double rl = 100.0 * (double)railed    / (double)n;
            const double qt = 100.0 * (double)quiet     / (double)n;
            const bool bad = nonFinite > 0 || rl > 20.0 || qt > 95.0;
            if (bad) snprintf(out, cap, "AUDIO AUDIT: nonfinite %.1f%% railed %.1f%% quiet %.1f%% (n=%lld)%s%s",
                              nf, rl, qt, n,
                              stage ? " ORIGIN=" : "", stage ? stage : "");
            n = nonFinite = railed = quiet = 0; tMs = nowMs;
            return bad;
        }
    };
    AudioAudit audit;

    void onAudio(stereo_t* data, int count, int ch) {
        if (count <= 0) return;
        {
            for (int i = 0; i < count; i++) { audit.note(data[i].l); if (ch == 2) audit.note(data[i].r); }
            char line[160];
            const double nowMs = (double)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            // ★ ORIGIN= names the pipeline stage that first went non-finite. Guards make the
            // fault self-healing, but healing is not diagnosis — this is what identifies the
            // stage that actually PRODUCES the NaN, so it can be fixed at source.
            if (audit.tick(nowMs, line, sizeof line, rx.faultStage())) LOGI("%s", line);
        }
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
        rdsPsName.clear(); rdsText.clear(); rdsPi = -1; rdsEcc = 0; rdsBer = -1; rdsSig = -99.0f;
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
        cb.rdsPi    = &Impl::rdsPiCb;
        cb.rdsBer   = &Impl::rdsBerCb;
        cb.rdsSig   = &Impl::rdsSigCb;
        cb.rdsExt   = &Impl::rdsExtCb;
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
        std::string ps, rt; int pi = -1, ecc = 0, ber = -1; float sig = -99.0f;
        bool wfm = (mode == "wfm");
        if (wfm) {
            std::lock_guard<std::mutex> lk(rdsMtx);
            ps = rdsPsName; rt = rdsText; pi = rdsPi; ecc = rdsEcc; ber = rdsBer; sig = rdsSig;
        }
        // trim trailing spaces RDS pads with
        auto trim = [](std::string s){ size_t e = s.find_last_not_of(" \t\r\n"); return e==std::string::npos?std::string():s.substr(0,e+1); };
        ps = trim(ps); rt = trim(rt);
        const bool st = wfm && stereoDetected.load();
        // Only send when something actually CHANGED — re-sending identical RDS each
        // second re-triggers the client's notification marquee (text "repopulates"
        // and flickers). Change-detect ps/rt/pi/ecc/stereo and skip otherwise.
        // ★ BER counts as a change TOO — otherwise the one case that most needs reporting
        // is the one that never reports. A station decoding nothing has no name, no PI and
        // no stereo transition, so the whole message was suppressed and the client could
        // not tell "the decoder is receiving damaged blocks" from "the decoder is not
        // running at all". Those are opposite faults and they looked identical.
        // The client must not re-trigger its marquee on a BER-only change (it keys that off
        // ps/rt), so this is safe to send at the 1 Hz metadata cadence.
        if (ps == lastSentPs_ && rt == lastSentRt_ && pi == lastSentPi_ && ecc == lastSentEcc_
            && st == lastSentStereo_ && ber == lastSentBer_
            && std::fabs(sig - lastSentSig_) < 0.5f) return;
        lastSentPs_ = ps; lastSentRt_ = rt; lastSentPi_ = pi; lastSentEcc_ = ecc; lastSentStereo_ = st;
        lastSentBer_ = ber; lastSentSig_ = sig;
        char buf[512];
        snprintf(buf, sizeof buf,
            "{\"type\":\"rds\",\"stereo\":%s,\"ps\":\"%s\",\"radiotext\":\"%s\",\"pi\":%d,\"ecc\":%d,\"ber\":%d,\"sig\":%.1f}",
            st ? "true" : "false",
            jsonEscape(ps).c_str(), jsonEscape(rt).c_str(), pi, ecc, ber, sig);
        sendText(sock, buf);
    }
    float lastSentSig_ = -999.0f;
    int lastSentBer_ = -2;   // -2 = never sent (distinct from -1 = decoder has no window)
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
        const int cfgBins = g_vsOutBins.load();
        double binBw = effective / (double)cfgBins;                    // we emit cfgBins bins (the FFT/bin lever)
        char buf[384];
        // maxBandwidth = full (unzoomed) device span — the client caps zoom-out
        // to this so you can't zoom out past the actual RTL bandwidth.
        // ★ mode: the server is AUTHORITATIVE on its own starting demodulator (the owner sets it,
        // and it is configurable). Without it the web client defaulted to nfm while the server ran
        // wfm — the UI showed NFM with a thin NFM passband until you clicked a mode. The client
        // adopts this on the first config when it has no remembered session.
        snprintf(buf, sizeof buf,
            "{\"type\":\"config\",\"centerFreq\":%lld,\"binCount\":%d,"
            "\"binBandwidth\":%.6f,\"totalBandwidth\":%.1f,\"maxBandwidth\":%.1f,\"mode\":\"%s\"}",
            (long long)llround(viewCenter.load()), cfgBins, binBw, effective, span, mode.c_str());
        sendText(sock, buf);
    }

    // Waterfall zoom: set the FFT-crop factor to match the requested span
    // (binBandwidth*fftSize). Pure display-side crop in onFFT — no IQ
    // decimation, no IQFrontEnd reconfig (which would touch the uninitialised
    // headless core), no effect on audio. Capped so the crop keeps >= 16 bins.
    void setSpan(double binBw) {
        if (binBw <= 0) return;
        // ★★ THE CLIENT'S OWN BIN COUNT, not the constant. The client derives its
        // requested span as binBw x (the bins IT receives), which sendConfig()
        // computes from g_vsOutBins — so interpreting it here with the fixed
        // OUT_BINS is only correct while the client happens to take all 4096.
        // The moment a client asks for fewer (the phone at 1024, Jr at 128) the
        // two disagree by exactly that ratio and every zoom lands 4x — or 32x —
        // off, which reads as erratic zoom and a waterfall that keeps rescaling.
        //
        // ★ It agreed by accident for years because only Jr used the bins lever
        // and its zoom is driven differently. Both sides must use the SAME bin
        // count: the one actually being sent.
        double want = sampleRate / (binBw * (double)g_vsOutBins.load());
        // ★★ ZOOM IS CAPPED BY REAL RESOLUTION, not by an arbitrary fraction.
        // The old /16 left just 16 SOURCE bins spread across ~1024 output bins —
        // a ~9 kHz span on a 2.4 MHz capture, which is 64x interpolation and
        // renders as mush (Stuart, 2026-07-26: "pretty useless"). Zooming past
        // the point where the FFT has bins to show is magnifying nothing.
        //
        // /64 keeps at least 64 source bins on screen (~37 kHz span at 2.4 MHz,
        // ~586 Hz/bin) — still far inside an FM channel, and everything shown is
        // measured rather than interpolated.
        //
        // ★ To zoom DEEPER honestly, raise fftSize; do not raise this. Resolution
        // is a property of the FFT, and no cap can invent it.
        double maxZoom = (double)fftSize / 64.0;
        if (want < 1.0) want = 1.0;
        if (want > maxZoom) want = maxZoom;
        zoomFactor.store(want);
    }

    static bool recvN(const std::shared_ptr<net::Socket>& s, uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) { int r = s->recv(buf+got, n-got, true, net::NO_TIMEOUT); if (r <= 0) return false; got += (size_t)r; }
        return true;
    }
    // idleMs: if not NO_TIMEOUT, the FIRST header byte read honours this timeout. On timeout (the
    // client is quiet but the socket is still open) returns -2, so the caller can run a heartbeat;
    // a genuine close still returns -1. Default keeps the original blocking behaviour.
    int recvWs(const std::shared_ptr<net::Socket>& s, std::string& out, int idleMs = net::NO_TIMEOUT) {
        uint8_t h[2];
        int r = s->recv(&h[0], 1, false, idleMs, nullptr);   // first byte, timeout-aware
        if (r < 0) return -1;
        if (r == 0) return s->isOpen() ? -2 : -1;            // -2 = idle (still open), -1 = closed
        if (!recvN(s, &h[1], 1)) return -1;
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
        // ★ LOGGED alongside fftRate: these are the TWO ways a client can ask to be slowed, and
        //   which one it uses depends on whether it has recognised this server as a VibeServer.
        //   The app showed a POWER SAVE pill while sending NEITHER (Stuart, 2026-08-01), so the
        //   question is not "was it honoured" but "which path did it take, if any".
        if (type == "set_rate") {
            if (jsonNum(msg,"divisor",v)) {
                LOGI("client asked for divisor %d", (int)llround(v));
                rateDivisor.store(std::max(1,(int)llround(v)));
            }
            return;
        }
        // ★ RSP-specific controls. They belong HERE, on the spectrum socket, because that is
        // the connection the client's control messages travel on — the first attempt put them
        // in the audio/decoder loop, where nothing ever sent them, so every RSP control
        // silently did nothing (Stuart, 2026-07-26).
        // ★ One message for the lot: these exist on one kind of radio, so they have no place
        // in the shared tune/gain vocabulary that every source must understand.
        // ★ Airspy HF+ controls, on the SPECTRUM socket like the RSP ones — that is the
        // connection a client's control messages travel on. The first RSP attempt put them in
        // the audio loop where nothing ever sent them, and every control silently did nothing.
        // ★★★ ADMIN UNLOCK. HMAC(adminSecret, nonce) — the same challenge-response the PIN
        // uses, so the password itself never crosses the wire, and the same nonce endpoint
        // issues the challenge. Reusing it rather than inventing a second scheme also means it
        // inherits the failure lockout that already exists.
        // ★★★ THE GATE, AND IT LIVES ON THE SERVER. Hiding a control in the client is
        // cosmetic: anything can open the WebSocket and send `biast` directly. So the refusal
        // has to be here, and the client's hiding is only a courtesy on top of it.
        // ★ WHAT IS PROTECTED, and why these and not others:
        //   • bias-T — puts DC ON THE FEEDLINE. A stranger flipping it can damage whatever is
        //     connected. Not "advanced", actively dangerous.
        //   • direct sampling — reconfigures the whole front end; a listener who leaves it on
        //     makes the receiver look broken to everyone after them.
        //   • PPM / calibration — silently miscalibrates the radio, and the damage is invisible
        //     and persistent. Everyone who follows quietly gets wrong frequencies.
        // ★ NOT protected: gain, sample rate, tuning, mode, and the per-listener DSP. Those are
        // the controls someone needs to actually USE a receiver, they are recoverable in a
        // click, and locking them would make a public server pointless (Stuart, 2026-07-27:
        // Hans "will probably only want the gain and sample rate accessible").
        auto adminGate = [this](const char* what) -> bool {
            bool needed;
            { std::lock_guard<std::mutex> lk(g_vsAdminMtx); needed = !g_vsAdminSecret.empty(); }
            if (!needed || adminOk.load()) return true;
            LOGI("refused %s — admin password required", what);
            std::shared_ptr<net::Socket> sc;
            { std::lock_guard<std::mutex> lk(clientMtx); sc = specClient; }
            if (sc) sendText(sc, "{\"type\":\"admin\",\"ok\":false,\"refused\":true}");
            return false;
        };
        if (type == "admin_unlock") {
            std::string secret;
            { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
            const std::string nonce = jsonStr(msg, "nonce");
            const std::string token = jsonStr(msg, "token");
            const std::string ip = sock ? sock->peerAddress() : "";
            bool ok = false;
            if (secret.empty()) {
                // No password configured: nothing is protected, so "unlocked" is the honest
                // answer rather than refusing a request that has nothing to refuse.
                ok = true;
            } else if (!g_vsAuthState.blocked(ip) && !nonce.empty() && !token.empty()
                       && g_vsAuthState.verify(secret, nonce, token)) {
                ok = true; g_vsAuthState.recordOk(ip);
            } else {
                g_vsAuthState.recordFail(ip);
            }
            adminOk.store(ok);
            LOGI("admin unlock %s", ok ? "granted" : "REFUSED");
            std::shared_ptr<net::Socket> sc;
            { std::lock_guard<std::mutex> lk(clientMtx); sc = specClient; }
            if (sc) sendText(sc, ok ? "{\"type\":\"admin\",\"ok\":true}"
                                    : "{\"type\":\"admin\",\"ok\":false}");
            return;
        }
        if (type == "ahf_control") {
            if (jsonNum(msg, "att", v))    LocalSdrShim::instance().setAhfAttenuation((int)v);
            if (jsonNum(msg, "lna", v))    LocalSdrShim::instance().setAhfLna(v != 0);
            if (jsonNum(msg, "thresh", v)) LocalSdrShim::instance().setAhfAgcThreshold(v != 0);
            // ★ Calibration is protected even though the rest of this message is not:
            // gain a listener can play with, a miscalibrated reference they cannot see.
            if (jsonNum(msg, "ppb", v) && adminGate("calibration"))
                LocalSdrShim::instance().setAhfCalibrationPpb((int)v);
            // ★ AGC LAST, matching the client's own send order: it owns the gain path, so
            // applying it before the manual attenuation would let it immediately override it.
            if (jsonNum(msg, "agc", v))    LocalSdrShim::instance().setAhfAgc(v != 0);
            return;
        }
        if (type == "rsp_control") {
            if (jsonNum(msg, "lna", v))      LocalSdrShim::instance().setLnaState((int)v);
            if (jsonNum(msg, "ifgr", v))     LocalSdrShim::instance().setIfGainReduction((int)v);
            if (jsonNum(msg, "ifagc", v))    LocalSdrShim::instance().setIfAgc(v != 0);
            if (jsonNum(msg, "agcset", v))   LocalSdrShim::instance().setIfAgcSetPoint((int)v);
            // Loop dynamics arrive together — they only make sense as a set.
            {
                double a, d, dd, th;
                if (jsonNum(msg, "agcAttack", a) && jsonNum(msg, "agcDecay", d)
                 && jsonNum(msg, "agcDelay", dd) && jsonNum(msg, "agcThresh", th))
                    LocalSdrShim::instance().setIfAgcDynamics((int)a, (int)d, (int)dd, (int)th);
            }
            if (jsonNum(msg, "rfnotch", v))  LocalSdrShim::instance().setRfNotch(v != 0);
            if (jsonNum(msg, "dabnotch", v)) LocalSdrShim::instance().setDabNotch(v != 0);
            // ★ The RSP has its own bias-T, and it is the same hazard as the dongle's.
            if (jsonNum(msg, "biast", v) && adminGate("bias-T"))
                LocalSdrShim::instance().setBiasT(v != 0);
            return;
        }
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
                    ? bb * (double)g_vsOutBins.load()   // same reason as setSpan()
                    : displaySpan() / zoomFactor.load();
                double dongle = dongleForView(v, viewSpan);
                bool moved = std::fabs(dongle - rtlCenter.load()) > 1.0;
                // Cooldown: a hardware retune is a PLL relock, and doing it on every
                // pan message audibly breaks the audio. The crop keeps the display
                // honest in between, so skipping a retune costs nothing visible.
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                // ★★★ DEFER, DO NOT DROP. This used to skip the retune entirely when the
                //     cooldown was still running, and NOTHING RETRIED IT — so `viewCenter` (stored
                //     unconditionally, just above) moved while the radio stayed put, and the crop
                //     `viewCenter − rtlCenter` was computed against an rtlCenter that never caught
                //     up. Every station then drew in the wrong place.
                //     ★ It bites hardest exactly where it is least acceptable: ON CONNECT. The
                //     client restores its zoom and its tune milliseconds apart, so the first move
                //     is allowed and STARTS the cooldown, and the second is discarded — no panning
                //     required, VFO locked or not (Stuart, 2026-08-02: "I never moved the view
                //     centre, I always keep the vfo locked", and stations offset on every fresh
                //     connect until he "tuned just a little" and they "snapped back").
                //     ★★ The cooldown itself is right. What was wrong was throwing the request
                //     away instead of remembering it — flushed by the DSP loop the moment the
                //     window expires (see flushPendingDongle).
                if (moved) {
                    if (nowMs - lastDongleMoveMs >= 120) {
                        lastDongleMoveMs = nowMs;
                        pendingDongle = 0.0;
                        rtlCenter.store(dongle);
                        tuneHw(dongle);
                        rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
                    } else {
                        pendingDongle = dongle;   // the newest request wins; applied shortly
                    }
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
            if (!adminGate("bias-T")) return;
            LocalSdrShim::instance().setBiasTee(msg.find("\"on\":true") != std::string::npos); return;
        }
        if (type == "agc") {
            LocalSdrShim::instance().setAgc(msg.find("\"on\":true") != std::string::npos); return;
        }
        if (type == "ppm") {
            if (!adminGate("ppm")) return;
            if (jsonNum(msg,"value",v)) LocalSdrShim::instance().setPpm((int)v); return;
        }
        // Capture sample rate = the spectrum span the server sends. A remote client
        // can widen/narrow it (e.g. drop the rate to ease a struggling link) without
        // touching the server. setSampleRate restarts the IQ stream and pushes a
        // fresh config, so the client's waterfall span updates itself.
        // The rate cap is an UP-TO ceiling, NOT a lock: a listener may pick a LOWER rate (narrower
        // span, less CPU) but never widen ABOVE the host's max. This is the enforcement (clamp),
        // not the UI — an old or hand-rolled client that asks for more is clamped down, not obeyed.
        if (type == "sampleRate") {
            // ★★★ REFUSED ON AN HF+. See the lockedRate note in hwinfo: changing an HF+'s rate on a
            //     live stream is a path no other SDR client takes, and ours could leave the device
            //     needing a power cycle. The client hides the picker; this is the enforcement, for
            //     an old or hand-rolled client that asks anyway.
            if (LocalSdrShim::instance().isAirspyHf()) {
                LOGI("sampleRate ignored — the Airspy HF+ is pinned to the rate it opened at");
                return;
            }
            if (jsonNum(msg,"value",v) && v > 0) {
                const double maxR = g_serveOnLan.load() ? g_vsLockedRate.load() : 0.0;
                if (maxR > 0 && v > maxR) v = maxR;   // clamp to the ceiling; lower is allowed
                LocalSdrShim::instance().setSampleRate(v);
            }
            return;
        }
        if (type == "directSampling") {
            if (!adminGate("direct sampling")) return;
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
        // ★★ THE ANALYSER SWITCH, ON THE CONTROL SOCKET. The web client turns the extended
        // RDS stream on by ATTACHING A DECODER, which is right for it — it already holds the
        // decoder websocket for every other extension. The phone does not: it would have to
        // open, and keep alive, a second socket solely to set one boolean. So the same switch
        // is offered here. Both paths set exactly the same two things, and the semantics are
        // unchanged: while nobody is looking, neither the extra CPU nor the extra bytes are
        // spent. Do not let these two drift apart.
        if (type == "rdsx") {
            const bool on = jsonNum(msg, "on", v) && v != 0.0;
            rdsxOn.store(on);
            rx.setRdsNoiseCorrection(on);   // honest deviation readout, only while it is read
            return;
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
            // ★ LOGGED, because a client can SAY it has slowed down and not have. The app showed
            //   "POWER SAVE - spectrum slowed" while the status line read 14 fps and 23 kB/s
            //   (Stuart, 2026-08-01) — the pill and the wire disagreeing, with nothing to say
            //   which side failed. The server sees the truth: either the 5 arrives here or it
            //   never left.
            if (jsonNum(msg, "value", v) && v > 0) {
                LOGI("client asked for %.0f fps", v);
                LocalSdrShim::instance().setFftRate(v);
            }
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
        // ★★ THE RATES THIS RADIO CAN ACTUALLY DO. An RSP is not a dongle: it runs to
        // 10 MSPS where the RTL2832U tops out near 2.56, and offering the dongle's list on
        // an RSP wastes most of the hardware while implying it is the limit. The client
        // renders whatever we send, so the honest answer is per-device (2026-07-26).
        if (LocalSdrShim::instance().isSdrplay())
            // ★★ NOTHING BELOW 2 MHz. The RSP runs ZERO-IF only at 2 MHz and above; narrower
            // spans need a LOW-IF mode (450 kHz / 1.62 / 2.048 MHz) with its own bandwidth
            // rules, or the ADC left at a legal rate and the API's DECIMATION used to reach
            // the output rate. We do neither yet, so offering 1 MHz would advertise a span
            // the radio cannot legally produce in the mode we configure (Stuart, 2026-07-26).
            // ★ Advertising a capability we have not implemented is worse than omitting it:
            // it fails at the radio, where it looks like broken hardware.
            // ★ NO 10 MSPS EITHER, and for exactly the reason 3.2 is missing from the RTL
            // list above: measured on air, 10 MHz is "a broken mess" while 8 works well
            // (Stuart, 2026-07-26). The API accepts it and then fails to sustain it — and
            // being the biggest number in the list, it is the first one a curious user
            // reaches for, so the dropped samples read as a bad receiver rather than a bad
            // setting. Offer the real ceiling instead.
            j += "],\"rates\":[8000000,6000000,5000000,4000000,3000000,2048000,2000000]";
        else if (useAirspyHf()) {
            // ★★★ THE RADIO'S OWN LIST, not ours. An HF+ Discovery tops out near 912 kHz where
            // the dongle list starts at 960 kHz — so EVERY rate we were offering was impossible,
            // and the picker was showing a list the hardware would refuse (Stuart, 2026-07-27:
            // "still got the RTL sample rates").
            // ★ FOURTH time this exact shape has bitten today: `if (isSdrplay()) ... else
            // <dongle>`. A two-source world written as "the other one" mis-handles the third
            // EVERY time — see radioCapsJson and resumeCaptureIdle. Name every source.
            j += "],\"rates\":[";
            const auto& rl = ahf->sampleRates();
            // Descending, to match the order the other two lists use — the client shows them
            // in the order given and a list that runs the other way looks like a different
            // control.
            for (size_t i = rl.size(); i-- > 0; )
                j += std::to_string(rl[i]) + (i ? "," : "");
            j += "]";
        }
        else
            j += "],\"rates\":[2560000,2400000,1800000,1200000,960000]";
        // ★ And WHICH radio, plus the controls it really has. A single gain slider is a lie
        // on an RSP: RF gain is an LNA STATE and IF gain is a separate REDUCTION, and it is
        // the LNA that decides whether the front end overloads — the very thing that has been
        // destroying RDS all evening. A client cannot present that honestly unless it is
        // told, so it is told.
        j += LocalSdrShim::instance().radioCapsJson();
        // ★★ IS THERE AN ADMIN PASSWORD, AND ARE WE THROUGH IT? Advertised for the same reason
        // as everything else here: the server ENFORCES the lock (bias-T, PPM, direct sampling
        // and calibration all go through adminGate), but a client that is not told simply draws
        // the controls as normal and the user finds out only when one silently does nothing
        // (Stuart, 2026-07-27: "all controls for the SDR still present"). Enforcement without
        // advertisement is a protection nobody can see.
        { bool aset;
          { std::lock_guard<std::mutex> al(g_vsAdminMtx); aset = !g_vsAdminSecret.empty(); }
          j += std::string(",\"adminSet\":") + (aset ? "true" : "false");
          j += std::string(",\"adminOk\":")  + (adminOk.load() ? "true" : "false"); }
        // ★★ THE COUNTDOWN NEEDS A DEADLINE AT CONNECT, not just the two warnings. The first
        // cut drove the client's timer ENTIRELY from session_warning at T-120 and T-30 — so on
        // a 30-minute limit the listener saw nothing at all for 28 minutes and concluded the
        // limit had not taken (Stuart, 2026-07-27, connected from his Mac). The warnings are
        // the nudge; this is the clock.
        // -1 = no limit, or this listener is exempt (loopback / admin).
        { const int left = LocalSdrShim::instance().occupantSecsLeft();
          j += ",\"sessionLimitMin\":" + std::to_string(g_vsSessionLimitMin.load());
          j += ",\"sessionSecsLeft\":" + std::to_string(left); }
        // A pinned rate is advertised so the client can HIDE its rate picker and say
        // who set it, rather than offering a control whose every use is silently
        // dropped. 0 = client-controlled (the default).
        { double lr = g_serveOnLan.load() ? g_vsLockedRate.load() : 0.0;
          // ★★★ THE AIRSPY HF+ IS PINNED TO THE RATE IT OPENED AT, and it advertises that here so
          //     the picker disappears rather than offering a control that can wedge the radio.
          //     ★ NOBODY CHANGES AN HF+'s RATE ON A LIVE STREAM. Checked against the field:
          //       • SDR++ (mainline AND Brown) DISABLE the rate combo while running — you stop the
          //         device, choose, and start; start() re-applies rate -> freq -> gains -> start.
          //       • gr-osmosdr sets it once at flowgraph construction and THROWS on an unsupported
          //         rate rather than snapping to a neighbour.
          //       • OpenWebRX is profile-based and does not change rate on the fly at all (Stuart).
          //     Our live-reconfigure path was therefore one nobody else exercises, and it showed:
          //     mis-tuning that only a manual retune cleared, audio a full span off with an image
          //     beside it, and on one occasion a USB endpoint wedged hard enough to need a reboot
          //     of the host (2026-08-01/02).
          //     ★★ Stuart's call, and the right one: "we cannot have users wedge a device that
          //     shouldn't really have its sample rate changed on the fly." A control that can
          //     brick the receiver until it is power-cycled is not a feature.
          //     ★ The rate is still the RADIO'S choice at open (the config file sets it), so an
          //     owner who wants 456 kHz sets it there and restarts the server — the same
          //     stop-and-start every other client requires, just expressed as configuration.
          if (useAirspyHf() && sampleRate > 0) lr = sampleRate;
          j += ",\"lockedRate\":" + std::to_string((long long)(lr > 0 ? lr : 0)); }
        // THE FRAME-RATE CEILING, for the same reason lockedRate is advertised: a client that
        // asks for more than the owner allows is SILENTLY CLAMPED (setFftRate, and the start
        // path), and silence is the worst possible answer for an ADAPTIVE client. A rate
        // controller that can't see the ceiling reads "I asked for 20 and got 10" as a failing
        // link and keeps stepping down chasing a limit it can never reach.
        // 0 here = the server's default (20 fps), i.e. no owner-imposed cap.
        { double mr = g_serveOnLan.load() ? g_vsMaxFftRate.load() : 0.0;
          j += ",\"maxFftRate\":" + std::to_string((long long)(mr > 0 ? mr : 0)); }
        // ★★ NR AND NOTCH ARE GLOBAL AND STICKY — the same reason lockedRate is here.
        // They live on the Impl and survive a listener leaving, so the NEXT listener
        // inherits whatever the last one set while their own UI renders its defaults.
        // Stuart tuned to MW, heard something odd, and found NR shown OFF and actually
        // ON — only "wiggle the control" resynced it (2026-07-28). A control that lies
        // about the radio's state is worse than one that is missing: you cannot even
        // tell something is wrong.
        // ★ ADVERTISE STATE THE SERVER ENFORCES. Every silent disagreement we have hit
        // — locked rate, fps ceiling, admin lock, and now these — is the same bug.
        j += std::string(",\"nr\":")    + (nrOn.load()    ? "true" : "false");
        j += std::string(",\"notch\":") + (notchOn.load() ? "true" : "false");
        // Owner requires the idle saver — the client locks its toggle on rather than offering a
        // switch we would silently ignore.
        j += ",\"forceIdleSaver\":";
        j += (g_serveOnLan.load() && g_vsForceIdle.load()) ? "1" : "0";
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
        // THE MACHINE RUNNING THE SERVER NEVER NEEDS THE PIN. The PIN controls who on the NETWORK
        // may use your radio; the person sitting at the host is the operator who set it. Making
        // them type their own PIN to listen on their own Mac is friction that teaches people to
        // pick weak ones — and anyone already running code on this machine can read the config
        // file anyway, so it concedes nothing.
        if (isLoopback(ip)) return true;
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
            // ★★ A NONCE EVEN WITH NO PIN. This used to answer a bare {"required":false},
            // which is correct for the PIN and useless for everything else — and the very
            // configuration a PUBLIC receiver wants is NO PIN (everyone may listen) WITH an
            // admin password (nobody may touch the hardware). Without a nonce there, the admin
            // override had nothing to sign and would have had to send the password itself.
            // ★ Issuing one costs nothing and tells an attacker nothing: it is random, single
            // use, and worthless without the secret.
            if (secret.empty())
                body = "{\"required\":false,\"nonce\":\"" + g_vsAuthState.issue() + "\"}";
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
            // FFT/bin lever: a spectrum client may ask for fewer output bins (?bins=N) to shrink each
            // SPEC frame — the watch, ~200 px wide over Bluetooth, needs far fewer than the web's full
            // res. No param → full res (OUT_BINS), so the web client is unaffected. Clamp [128, OUT_BINS].
            // ★★ DO NOT APPLY IT HERE. This runs on the HTTP upgrade, BEFORE the
            // occupancy check — so a client that is about to be refused
            // {"type":"busy"} was still rewriting the bin count out from under
            // the CLIENT ALREADY STREAMING. Jr asks for 128; the incumbent phone,
            // which sends no param and expects the full 4096, went blocky
            // mid-session because somebody else merely TRIED to connect.
            //
            // A refused client must change nothing about the server. Resolve the
            // value here, apply it inside acceptWs once the slot is actually won.
            //
            // ★ Still a GLOBAL, and that is the deeper flaw: bins are per-client
            // state living in one variable, which only holds while exactly one
            // client streams. Real multi-client has to make this per-connection.
            int wantBins = 0;
            if (wsSpec) {
                const std::string bq = queryParam(reqLine, "bins");
                wantBins = bq.empty() ? OUT_BINS : atoi(bq.c_str());
                if (wantBins < 128) wantBins = 128; else if (wantBins > OUT_BINS) wantBins = OUT_BINS;
            }
            // ★ The admin password may ride the connect URL, because an override has to be
            // decided BEFORE the slot is claimed — the existing admin_unlock message arrives
            // over a socket the client cannot open while the server is telling it "busy".
            // ★ Admin override rides the connect URL as a nonce + HMAC pair — never the
            // password — because the override has to be decided BEFORE the slot is claimed,
            // and the admin_unlock message arrives over a socket a busy server will not open.
            acceptWs(sock, wsKey, wsAudio, queryParam(reqLine, "user_session_id"),
                     queryParam(reqLine, "codec") == "opus",
                     queryParam(reqLine, "channels") == "1", wantBins,
                     queryParam(reqLine, "vs_admin_nonce"),
                     queryParam(reqLine, "vs_admin_auth"));
        } else if (reqLine.find("/connection") != std::string::npos) {
            // Preflight for a manually-added server (the phone/web asks before opening sockets).
            // Report occupancy HERE so a full server says "in use, try again later" up front,
            // instead of the client opening a socket only to be refused with type:"busy". A
            // loopback caller (the host's own browser) is never told it is busy — it IS the
            // occupant or is about to become one.
            // ★★ ASK WHO IS CALLING. acceptWs() lets the occupant back in
            // (`occupantSession != me`), but this preflight compared against
            // NOBODY — so a client whose own audio socket already held the slot
            // was told its own server was in use, and its spectrum never opened.
            // Audio playing while the app reports "in use" is the signature.
            //
            // ★ The id must come from the QUERY STRING: only the request line is
            // available here, so an id sent in the POST body cannot be seen. Old
            // clients send nothing, and fall back to the previous behaviour.
            const std::string me = queryParam(reqLine, "user_session_id");
            bool busy;
            { std::lock_guard<std::mutex> lk(clientMtx);
              busy = !occupantSession.empty()
                     && (me.empty() || occupantSession != me)
                     && ((specClient && specClient->isOpen()) || (audioClient && audioClient->isOpen())); }
            // Loopback exemption retained for the host's own browser, which is
            // the occupant or about to become one. It was also masking the bug
            // above, which is why only NETWORK clients ever saw it.
            if (busy && isLoopback(sock->peerAddress())) busy = false;
            std::string body = busy
                ? "{\"allowed\":false,\"reason\":\"in-use\"}"
                : "{\"allowed\":true}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /vibeserver.json", 0) == 0) {
            // ★★ POSITIVE IDENTITY. detectServerType() used to sniff the landing
            // page for the substring "vibeserver" — but serving that page is
            // OPTIONAL (--no-web / webServer:false), and with it off `GET /`
            // returns a page saying only "VibeSDR", the CLIENT's name. The
            // detector must never match "vibesdr" (that is what mis-typed real
            // UberSDR servers as VibeServer in v8.0.0), so a web-disabled
            // VibeServer fell through to the `return 'ubersdr'` default EVERY
            // TIME. This is the one protocol we own both ends of, so it should
            // never be guessed at from prose.
            //
            // Deliberately OUTSIDE the g_vsWebEnabled gate: identity is not part
            // of the web client, and gating it would reintroduce the same bug.
            bool pinOn;
            { std::lock_guard<std::mutex> lk(g_vsMtx); pinOn = !g_vsSecret.empty(); }
            // ★ `uncompressed` is the OPERATOR's policy and `local` is this requester's
            // own situation; the client needs both to decide what to ask for and what to
            // offer in its menu. Sent from here rather than over the audio socket because
            // the decision has to be made BEFORE that socket is opened — the codec is a
            // query parameter on the connect URL.
            const int um = g_vsUncompressedAudio.load();
            const bool loop = isLoopback(sock->peerAddress());
            // ★ Advertised so the client can OFFER the unlock box only where there is something
            // to unlock — an unlock prompt on a server with no admin password is a puzzle.
            bool adminSet;
            { std::lock_guard<std::mutex> lk(g_vsAdminMtx); adminSet = !g_vsAdminSecret.empty(); }
            std::string body = std::string("{\"server\":\"vibeserver\",\"proto\":1,\"pin\":")
                             + (pinOn ? "true" : "false") + ",\"web\":"
                             + (g_vsWebEnabled.load() ? "true" : "false")
                             + ",\"uncompressed\":\""
                             + (um == 1 ? "choice" : um == 2 ? "compat" : "off")
                             + "\",\"local\":" + (loop ? "true" : "false")
                             + ",\"admin\":" + (adminSet ? "true" : "false")
                             // ★★ OCCUPANCY IN THE IDENTITY RESPONSE. The picker already fetches
                             // this for every known server, so an IN USE badge costs nothing
                             // extra — and a public receiver that is one-client-at-a-time has to
                             // say so BEFORE someone taps it, or every busy server looks broken.
                             + ",\"busy\":" + (LocalSdrShim::instance().isBusy() ? "true" : "false")
                             + ",\"limitMin\":" + std::to_string(g_vsSessionLimitMin.load())
                             // Seconds the current listener has left, -1 = no limit / free. Lets
                             // the picker say "free in 4 min" instead of a bare "in use", which
                             // is the difference between waiting and giving up.
                             + ",\"freeInSec\":" + std::to_string(LocalSdrShim::instance().occupantSecsLeft())
                             + "}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
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
        } else if (reqLine.rfind("GET /icon-512.png", 0) == 0) {
            std::string body((const char*)kVibeIcon512, kVibeIcon512Len);
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: max-age=3600\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /manifest.webmanifest", 0) == 0) {
            // PWA INSTALL. Lets a listener install the client as a real app — its own window, its
            // own dock/taskbar icon, no browser chrome — which suits "VibeServer is also a desktop
            // SDR" far better than a tab does.
            // ★ Install requires a SECURE CONTEXT, so this is offered on localhost (the
            // desktop-SDR case) but NOT to a LAN client over plain http://. That is a browser rule,
            // not ours.
            static const char* kManifest = R"JSON({
  "name": "VibeServer",
  "short_name": "VibeServer",
  "description": "Listen to this radio",
  "start_url": "/",
  "scope": "/",
  "display": "standalone",
  "background_color": "#080601",
  "theme_color": "#080601",
  "icons": [
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any" },
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "maskable" }
  ]
})JSON";
            std::string body(kManifest);
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/manifest+json\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Connection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /sw.js", 0) == 0) {
            // The minimum service worker that makes a site installable. It deliberately does NOT
            // cache: the client is served from the radio it controls, so a stale cached copy would
            // be a client talking to a server it no longer matches — the worst kind of bug to
            // debug. Chromium simply requires a fetch handler to exist.
            static const char* kSw =
                "self.addEventListener('install', e => self.skipWaiting());\n"
                "self.addEventListener('activate', e => e.waitUntil(self.clients.claim()));\n"
                "self.addEventListener('fetch', e => { /* always network: never serve a stale client */ });\n";
            std::string body(kSw);
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: text/javascript\r\n"
                          "Service-Worker-Allowed: /\r\n"
                          "Connection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /favicon", 0) == 0) {
            // A REAL file, not the data: URI in the page. Safari refuses data: URI
            // favicons outright and silently shows its own default arrow instead, so
            // the icon has to come from a URL. ~1 KB, compiled in beside the page.
            std::string body((const char*)kVibeFavicon, kVibeFaviconLen);
            // An hour, not a day. The icon is ~9 KB and served off the LAN, so a re-fetch costs
            // nothing — whereas a day-long hold means a changed icon does not appear until
            // tomorrow. The page also carries a ?v= cache key for exactly this reason.
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Cache-Control: max-age=3600\r\nConnection: close\r\nContent-Length: "
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
            // ★★★ NOT `std::string(kVibeWebPage)` — that is strlen, and the page contains NUL
            //     bytes (the WASM Opus decoder embeds its module as a binary string). It served
            //     233,787 bytes of a 488,109-byte page for exactly one deploy, with no error at
            //     either end. vibeWebPage() decodes base64 and knows its own length.
            const std::string& kPage = vibeWebPage();
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

    void acceptWs(std::shared_ptr<net::Socket> sock, const std::string& wsKey, bool isAudio,
                  const std::string& session, bool wantsOpus, bool forceMono = false,
                  int wantBins = 0, const std::string& adminNonce = "",
                  const std::string& adminToken = "") {
        std::string acc = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t digest[20]; Sha1().hash((const uint8_t*)acc.data(), acc.size(), digest);
        sock->sendstr("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + base64(digest, 20) + "\r\n\r\n");

        // ★ ONE OCCUPANT AT A TIME — and a second one is TURNED AWAY, not allowed to take over.
        //
        // The radio serves one listener until real multi-client lands
        // (files/BRIEF-vibeserver-protocol-foundations.md §4). The previous behaviour was TAKEOVER:
        // a new socket displaced the incumbent and closed it. But every client auto-reconnects on
        // close, so two of them (two browsers, or a Mac and a phone) closed each other forever — a
        // reconnect WAR where both could half-tune, audio stuttered, and the racing setSampleRate /
        // setFftRate calls from two control threads could corrupt the DSP rate state. So: reject.
        //
        // Occupancy is keyed on the client's session id, NOT its socket or IP: one browser opens a
        // spectrum AND an audio socket, both carrying the same id, and both must be let in. Two
        // browsers on the SAME machine share an IP but have different ids — which is why IP won't do
        // (that was the case that still fought). A socket with no id at all (an old client, or a raw
        // probe) is treated as its own anonymous occupant so it can't silently share the slot.
        {
            std::lock_guard<std::mutex> lk(clientMtx);
            const std::string me = session.empty() ? ("anon:" + sock->peerAddress()) : session;

            // ★★ COOLDOWN FIRST — before occupancy. Someone serving a cooldown must be refused
            // even when the radio is FREE; that is the entire point of it. Checking occupancy
            // first would let them straight back in the instant their own timeout freed the slot.
            // ★ Loopback is never on cooldown: it is never timed out in the first place.
            if (!isLoopback(sock->peerAddress())) {
                const double now = Impl::nowSecs();
                const auto it = cooldownUntil.find(sock->peerAddress());
                if (it != cooldownUntil.end()) {
                    if (it->second > now) {
                        const int left = (int)(it->second - now + 0.5);
                        LOGI("%s WS refused — cooling down %ds", isAudio ? "audio" : "spectrum", left);
                        const std::string m = "{\"type\":\"cooldown\",\"secs\":"
                                            + std::to_string(left) + "}";
                        sendWs(sock, 0x1, (const uint8_t*)m.data(), m.size());
                        sock->close();
                        return;
                    }
                    cooldownUntil.erase(it);   // expired — prune on the way past
                }
            }

            const bool occupied = !occupantSession.empty()
                && occupantSession != me
                && ((specClient && specClient->isOpen()) || (audioClient && audioClient->isOpen()));

            // ★★★ ADMIN OVERRIDE — the ONE case where takeover is allowed, and it must not
            // restart the reconnect war that made takeover the wrong default everywhere else.
            // Plain takeover failed because every client auto-reconnects on close, so two of
            // them displaced each other forever. The difference here is the DISPLACED client is
            // told WHY ("evicted"), which the clients treat as terminal and do not retry — and
            // an admin arriving is a deliberate, rare act by the owner, not a race between two
            // equal listeners.
            bool override_ = false;
            if (occupied && !adminNonce.empty() && !adminToken.empty()) {
                std::string secret;
                { std::lock_guard<std::mutex> al(g_vsAdminMtx); secret = g_vsAdminSecret; }
                // ★★★ CHALLENGE-RESPONSE, NEVER THE PASSWORD ITSELF. The first cut of this put
                // the admin password in the connect URL as a query parameter — which over plain
                // HTTP puts it in the clear on the wire, and into proxy and server logs along
                // the way. These receivers are going on the public internet, so that is exactly
                // the wrong shape (Stuart, 2026-07-27).
                // ★ Same VsAuth the PIN and admin_unlock already use: the server issues a nonce
                // at /auth, the client returns HMAC(secret, nonce), and the secret never
                // crosses the link. Reusing it also inherits the BRUTE-FORCE LOCKOUT — an
                // override endpoint without one is an open guessing gallery, and unlike the PIN
                // this one displaces a listener on success.
                const std::string ip = sock->peerAddress();
                override_ = !secret.empty()
                         && !g_vsAuthState.blocked(ip)
                         && g_vsAuthState.verify(secret, adminNonce, adminToken);
                if (override_) g_vsAuthState.recordOk(ip);
                else           g_vsAuthState.recordFail(ip);
                if (override_) {
                    LOGI("admin override — evicting the current occupant");
                    static const char* kEvict = "{\"type\":\"evicted\"}";
                    if (specClient  && specClient->isOpen())
                        { sendWs(specClient,  0x1, (const uint8_t*)kEvict, strlen(kEvict)); specClient->close(); }
                    if (audioClient && audioClient->isOpen())
                        { sendWs(audioClient, 0x1, (const uint8_t*)kEvict, strlen(kEvict)); audioClient->close(); }
                    occupantSession.clear();
                    // ★ NOT put on cooldown. They were evicted by the owner, not caught
                    // overstaying — punishing them for someone else's decision would be wrong.
                }
            }

            if (occupied && !override_) {
                // Tell them plainly, as a WS text frame (we have already upgraded), then close. The
                // client shows "in use, try again later" and must NOT retry-storm — see the web
                // client's handling of type:"busy".
                LOGI("%s WS refused — server busy (occupant present)", isAudio ? "audio" : "spectrum");
                sendWs(sock, 0x1,
                       (const uint8_t*)"{\"type\":\"busy\"}", 15);
                sock->close();
                return;
            }
            // ★ Start the clock on a NEW occupant only. A client opens two sockets (spectrum
            // and audio) and reconnects across blips with the same id — restarting the timer
            // on each would make the limit unenforceable, and resetting it on the second
            // socket of the same session would be a quiet bug nobody would ever see.
            if (occupantSession != me) {
                occupantSince   = Impl::nowSecs();
                occupantWarned  = 0;
                occupantAddr    = sock->peerAddress();
            }
            occupantSession = me;   // claim (or re-affirm) the slot for this client
            // ★ A NEW CLIENT IS NOT THE ADMIN. Clearing here means an unlock cannot outlive the
            // session that earned it and be inherited by whoever connects next.
            adminOk.store(false);
        }

        // ★ A client that cannot take Opus, on a server that does not allow raw,
        // is turned away HERE with a reason rather than being handed a stream it
        // cannot decode (silence) or one the owner cannot afford (187 KB/s).
        // ★★ Loopback is exempt IN EVERY MODE, including OFF — it is not a policy
        // exemption but a category error to apply the policy at all: this setting
        // rations the owner's UPLINK, and 127.0.0.1 does not touch it.
        if (isAudio && !wantsOpus && g_vsUncompressedAudio.load() == 0
            && !isLoopback(sock->peerAddress())) {
            LOGI("audio WS refused — uncompressed audio not allowed by the owner");
            static const char* kMsg = "{\"type\":\"needs_codec\",\"codec\":\"opus\"}";
            sendWs(sock, 0x1, (const uint8_t*)kMsg, strlen(kMsg));
            sock->close();
            return;
        }

        // Slot won — NOW it is safe to adopt this client's bin count. Before the
        // occupancy check, a refused client corrupted the incumbent's stream.
        if (wantBins > 0) g_vsOutBins.store(wantBins);

        // ★★ RESET PER-CLIENT RATE STATE ON ARRIVAL. `rateDivisor` is a global
        // that OUTLIVES the client that set it — nothing cleared it on connect or
        // disconnect. So a divisor left behind by a previous session silently
        // multiplied the next client's rate:
        //
        //     new client asks fftRate 2  ×  leftover divisor 3  =  0.67 fps
        //
        // It stayed hidden while every client also SENT a divisor (each one
        // overwrote the last). The moment a client expressed its rate purely as
        // fftRate instead, the stale value had nothing to overwrite it and the
        // stream ran at a third of the requested rate for no visible reason.
        //
        // ★ A fresh session must start from a known state, not inherit the last
        // one's. Same flaw as the bins global directly above.
        if (!isAudio) rateDivisor.store(1);

        // A listener has arrived — wake the dongle if it was idled while nobody was connected. Idempotent
        // (guarded by captureIdle), so whichever of the two sockets lands first does it. Only starts a
        // capture thread (no join), so it's safe here.
        resumeCaptureIdle();

        // ★ SAME-SESSION TAKEOVER. A client that resumes from background reconnects with its SAME
        // stable session id, but its PREVIOUS socket may still be a ghost here (watchOS suspended it
        // with no FIN, so our accept loop is still parked on it). The occupant check above lets the
        // same id back in — but if we just overwrite the pointer, the ghost's loop lingers and can
        // stall the reclaim. So grab the old same-role socket and CLOSE it: its loop unwinds at once,
        // and its cleanup below won't touch the new pointer. This is takeover only for the SAME
        // client (same id) — a DIFFERENT client is still refused as busy above.
        std::shared_ptr<net::Socket> stale;
        if (isAudio) {
            { std::lock_guard<std::mutex> lk(clientMtx);
              stale = audioClient;
              audioClient = sock; }
            audioForceMono.store(forceMono);
#ifdef VIBE_HAVE_OPUS
            audioWantsOpus.store(wantsOpus);
            opusEnc.reset();   // fresh stream for the new listener — no carried-over frame remainder
            LOGI("audio WS connected (codec=%s, channels=%s)", wantsOpus ? "opus" : "pcm",
                 forceMono ? "mono" : "native");
#else
            audioWantsOpus.store(false);   // no encoder in this build
            LOGI("audio WS connected (pcm, channels=%s)", forceMono ? "mono" : "native");
#endif
        } else {
            { std::lock_guard<std::mutex> lk(clientMtx);
              stale = specClient;
              specClient = sock; }
            sendConfig(sock); sendHwInfo(sock);
            LOGI("spectrum WS connected");
            // ★ TELL A NEW CLIENT THE CURRENT STATE. The device message is otherwise only sent when
            // the state CHANGES, so anyone who connected — or refreshed — after the dongle was
            // pulled saw a page that simply never updated, with nothing to explain it. Refreshing
            // is the first thing anyone tries; it must be the moment they learn what is wrong.
            if (deviceLost.load())
                sendText(sock, "{\"type\":\"device\",\"present\":false}");
        }
        // Boot the ghost (if any) now that the new socket has taken its place. Outside the lock:
        // close() only flips the old socket's flags/fd; its own accept loop does the bookkeeping.
        if (stale && stale != sock) stale->close();

        // ★ APPLICATION-LEVEL LIVENESS PING. A clean disconnect (FIN) is caught below by recvWs
        // returning -1 within a second. But a VANISHED peer — watchOS SUSPENDED Jr, or the link
        // dropped with no FIN — leaves the kernel connection alive (it may even keep ACKing), so no
        // TCP timeout fires and the server sat on a phantom "1 connection" indefinitely (Stuart,
        // 2026-07-23). The audio socket is server→client only, so the client never sends and we'd
        // block here forever. Instead, poll in 5s slices: when the client is quiet, PING it. A live
        // client (browser, or Jr with the app actually running — including background audio) answers
        // instantly and resets the clock; a suspended/gone one never does, so after 20s we drop it
        // and free the single-occupant slot. Background-audio Jr keeps ponging, so it is NOT dropped.
        auto monoMs = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        int64_t lastRx = monoMs();
        while (serverRunning.load() && sock->isOpen()) {
            std::string payload;
            int op = recvWs(sock, payload, 5000);
            if (op == -2) {                                   // quiet slice — probe liveness
                if (monoMs() - lastRx > 20000) {
                    LOGI("%s WS idle >20s (no pong) — dropping stale peer", isAudio ? "audio" : "spectrum");
                    break;
                }
                sendWs(sock, 0x9, nullptr, 0);                // ping
                continue;
            }
            if (op < 0 || op == 0x8) break;
            lastRx = monoMs();                                // any frame (incl. pong) = alive
            if (op == 0x9) { sendWs(sock, 0xA, (const uint8_t*)payload.data(), payload.size()); continue; }
            if (op == 0xA) continue;                          // pong — liveness only
            if (op == 0x1) handleControl(sock, payload);
        }
        bool bothGone = false;
        { std::lock_guard<std::mutex> lk(clientMtx);
          if (specClient == sock) specClient = nullptr;
          if (audioClient == sock) audioClient = nullptr;
          // Free the slot once BOTH of the occupant's sockets are gone (a browser closing one tab
          // drops both). Until then a momentary spectrum reconnect must not surrender the slot to a
          // waiting device. A refused socket never reached here (it returned early), so it can't
          // clear an occupancy it never held.
          const bool specGone  = !specClient  || !specClient->isOpen();
          const bool audioGone = !audioClient || !audioClient->isOpen();
          if (specGone && audioGone) { occupantSession.clear(); bothGone = true; } }
        sock->close();
        // No listeners → idle the dongle so an unattended server stops burning power. OUTSIDE the lock:
        // pauseCaptureIdle() joins the capture thread, which must never happen under clientMtx.
        // ★★★ BUT RE-CHECK FIRST — A NEW CLIENT MAY HAVE ARRIVED WHILE THIS SOCKET WAS CLOSING.
        //     resumeCaptureIdle() runs EARLY in the connect path, before the new client's pointers
        //     are stored, so the sequence
        //         [new] resume  →  [old] teardown sees no client  →  pause
        //     leaves the capture PARKED with a listener connected. The client then sits there with
        //     a socket that is open and silent: `audio 0 KB/s`, no waterfall, and a page refresh
        //     "fixes" it only because the next connect calls resume again (Stuart, 2026-08-02,
        //     switching from the app back to the web client).
        //     ★ Cheap and narrow: look again, under the lock, at the moment of pausing rather than
        //     acting on a decision taken a few microseconds earlier. Idling is never urgent — the
        //     next disconnect will park it if this one should not.
        if (bothGone) {
            bool stillEmpty;
            { std::lock_guard<std::mutex> lk(clientMtx);
              stillEmpty = (!specClient  || !specClient->isOpen())
                        && (!audioClient || !audioClient->isOpen()); }
            if (stillEmpty) pauseCaptureIdle();
            else LOGI("not parking — a new listener arrived while this socket was closing");
        }
        LOGI("%s WS disconnected", isAudio ? "audio" : "spectrum");
    }

    void startDecoder(const std::string& msg) {
        std::string ext = jsonStr(msg, "extension_name");
        // ★★ ADVANCED RDS. Not an audio decoder — it turns on the extended RDS stream (the
        // fields we normally discard, plus the constellation). It attaches through the same
        // path as every other decoder on purpose: SELECTING IT IS THE TOGGLE, so the extra
        // work and the extra bytes are paid for only while somebody is looking at them, and
        // there is no setting to explain (Stuart, 2026-07-26).
        if (ext == "rds") {
            rdsxOn.store(true);
            // ★ THE ANALYSER BEING OPEN IS THE SWITCH. The guard-band noise measurement exists
            // solely to make the DEVIATION READOUT honest, so it is worth its CPU exactly while
            // somebody is reading it — the same reasoning that gates the extended stream itself.
            // ★ It replaces an operator setting that also widened the channel filter; that half
            // was measured to cost 10 dB of RDS SNR and has been removed entirely.
            rx.setRdsNoiseCorrection(true);
            return;
        }
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
        // ★★★ autoSync OFF. The post-reception "Correcting slant..." pass is the only thing
        // breaking SSTV here: Stuart, 2026-08-01, watching a geometric test card (W6AOA's prism)
        // come in — "it receives it perfect, then its the cleanup pass afterwards that breaks it",
        // splitting the picture so the top third sits where the WHOLE image should be and the rest
        // stays put.
        //
        // ★★ And it should never have been running. Slant correction exists for SOUNDCARD CLOCK
        // DRIFT — a sample rate that is not quite what it claims. An SDR's clock is locked and its
        // rate exact, so there is no drift to correct; `redrawFromLuminance`'s own comments have
        // said so since the last attempt. The pass could only ever guess at a fault that was not
        // there. It defaulted to true and was exposed NOWHERE, so every frame got it.
        //
        // ★ Three fixes have now been aimed at making the correction behave (overrun guard, sync
        // confidence gate, offset-not-shear). Each was a real bug and none of them mattered,
        // because the whole pass is wrong for this input. Do not re-enable it without a soundcard
        // source to justify it AND the standalone harness to prove it.
        // ★★★ BACK ON, because the cleanup is no longer the thing that breaks the picture. It was
        //     switched off on 08-01 as the only way to stop a torn image — the right call at the
        //     time, but it left every picture with the wrap it was meant to remove.
        //     ★ What changed: the guard inside redrawFromLuminance now measures HOW FAR the
        //     correction overruns the captured audio instead of whether it overruns at all. A
        //     skip-sized overrun (bounded by one line) is applied; a drifting one still refuses.
        //     ★★ PROVEN ON REAL AUDIO, not one lucky frame: the four Essex Ham recordings
        //     (test/fixtures/sstv, Scottie S2 ×2 and Martin M2 ×2) went from 1 of 4 corrected to
        //     4 of 4, checked as images. `tools/sstv_harness.cpp` replays them on the Mac — run it
        //     before touching this again. Off-air remains the final word.
        sstv = new SstvDecoder(12000, /*autoSync=*/true);
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
        rdsxOn.store(false);
        rx.setRdsNoiseCorrection(false);   // nobody looking: stop paying for it
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
        // ★★★ ONLY TEAR DOWN IF WE ARE STILL THE CURRENT CLIENT — a DEPARTING socket must never
        // switch off state a NEWER one has already asked for.
        //
        // The old code guarded the POINTER against a newer client but tore the decoder and the
        // spots down unconditionally, which loses a race that a backgrounded browser tab runs
        // reliably:
        //   1. the tab is frozen, its socket drops, and this thread is scheduled to exit;
        //   2. the client reconnects 3s later, re-attaches `rds` and re-subscribes spots —
        //      the shim keeps no per-client state, so re-asserting is CORRECT and expected;
        //   3. only THEN does this tail run, calling stopDecoder() (rdsxOn = false) and
        //      stopSpots() — killing the new client's stream;
        //   4. `dxClient == sock` is false, so the pointer is left alone, which HID the damage.
        // The client is none the wiser: its socket is open and it was told "attached", so it
        // never retries. The Advanced RDS box goes BLANK and stays blank, and digital spots
        // stop — together, after the tab has been in the background (Stuart, 2026-07-27).
        //
        // ★★ Same shape as the per-client-state-in-globals family: state owned by the SERVER
        // but switched by whichever CLIENT happened to speak last. Guard the ACTION, not just
        // the bookkeeping that follows it.
        bool stillCurrent;
        { std::lock_guard<std::mutex> lk(clientMtx);
          stillCurrent = (dxClient == sock);
          if (stillCurrent) dxClient = nullptr; }
        if (stillCurrent) { stopDecoder(); stopSpots(); }
        sock->close();
        LOGI("dxcluster WS disconnected%s", stillCurrent ? "" : " (superseded — kept decoder)");
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

    // ★ COMPLEX FLOAT STRAIGHT IN. The Airspy HF+ hands libairspyhf's callback interleaved
    // float at roughly +/-1 — the engine's own format — so this path does no conversion at all.
    // Routing it through the int16 one would quantise an 18-bit-effective radio down to 16 and
    // straight back up, throwing away the dynamic range that is the entire reason to own one.
    void enqueueIqFloat(const float* interleaved, int sampCount, bool blockIfFull) {
        if (sampCount <= 0) return;
        if (sampCount > STREAM_BUFFER_SIZE) sampCount = STREAM_BUFFER_SIZE;
        std::vector<cf32> v((size_t)sampCount);
        std::memcpy(v.data(), interleaved, (size_t)sampCount * sizeof(cf32));
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
    /** ★ Apply a dongle move the cooldown postponed, once the window has passed. Called from the
     *  DSP loop, which already holds modeMtx — the same lock the pan handler takes, so there is
     *  exactly one hardware tune in flight at a time. Cheap: a comparison per buffer. */
    void flushPendingDongle() {
        if (pendingDongle <= 0.0) return;
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (nowMs - lastDongleMoveMs < 120) return;
        const double dongle = pendingDongle;
        pendingDongle = 0.0;
        if (std::fabs(dongle - rtlCenter.load()) <= 1.0) return;   // already there
        lastDongleMoveMs = nowMs;
        rtlCenter.store(dongle);
        tuneHw(dongle);
        rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
        LOGI("deferred retune applied: dongle -> %.0f Hz", dongle);
    }

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
            flushPendingDongle();     // a retune the pan cooldown postponed — never dropped
            rx.feed(buf.data(), (int)buf.size());
        }
    }

    // ── Dongle hot-plug: notice it go, take it back when it returns ─────────────────────────
    //
    // A server that keeps serving a page it can never fill is worse than one that says it is
    // broken. And on a headless Pi "unplug the dongle, reboot the box" is not a recovery story —
    // nobody is there to do it. So: tell the clients, then watch for the dongle coming back and
    // pick it up automatically.

    /** Start (or restart) the USB capture thread. ONE definition, so every path that relaunches
     *  capture also gets the loss detection — the sample-rate path originally did not, which would
     *  have silently disabled unplug detection for the rest of the session. */
    void launchCapture() {
        const uint32_t bufLen = rtlBufLenForRate(sampleRate);
        Impl* self = this;
        rtlThread = std::thread([self, bufLen]{
            vibeThreadName("vibe-rtl");
            rtlsdr_read_async(self->dev, &Impl::asyncHandler, self, 0, bufLen);
            // We only reach here when the IQ stops. Three reasons, and they must not be confused:
            if (self->stopping.load() || self->restarting.load()) return;   // we asked for it
            self->captureDown.store(true);
            // ★ VERIFY BEFORE ALARMING. If our dongle is still enumerable this was a stream fault,
            // not an unplug — the watchdog restarts it and the user need never know. Only when the
            // device has genuinely gone do we say so, because a false "no radio" over a working
            // waterfall destroys trust in every later warning.
            if (self->findOurDevice() < 0) {
                self->deviceLost.store(true);
                LOGE("RTL-SDR gone — unplugged or failed");
                self->notifyDeviceState();
            } else {
                LOGE("RTL-SDR stream stopped but the device is still present — restarting");
            }
        });
    }

    // ── Idle: stop the dongle when nobody is listening ────────────────────────────────────────────
    // With no client connected the capture + DSP were still grinding the dongle IQ into the void
    // (~3% CPU on an M4 — a real battery/thermal drain on a solar or old-Android appliance). Stop the
    // capture when the last listener leaves; `dspLoop` then PARKS on the empty IQ queue at ~0 CPU. We
    // set `restarting` for the whole paused period so the capture watchdog treats the stopped stream as
    // deliberate (it `continue`s on `restarting`) and never false-alarms "dongle gone" or relaunches.
    std::atomic<bool> captureIdle{false};
    /// ★★★ IDLE THE DONGLE BY DISCARDING, NOT BY STOPPING IT. See pauseCaptureIdle.
    std::atomic<bool> idleDiscard{false};
    void pauseCaptureIdle() {
        if (captureIdle.exchange(true)) return;               // already paused
        // ★★ EVERY SOURCE NAMED EXPLICITLY. The final `else` used to mean "must be a dongle",
        // which was true with two sources and silently wrong with three — see resumeCaptureIdle
        // for what that cost.
        if (useTcp()) { tcpRunning.store(false); if (rtlThread.joinable()) rtlThread.join(); }
        else if (useSdrplay()) { sdrp->setPaused(true); }
        else if (useAirspyHf()) { ahf->setPaused(true); }
        else {
            // ★★★ THE DONGLE IS NO LONGER STOPPED — IT IS IGNORED. Cancelling the async
            // stream and restarting it is what crashed the server, and the "fix" below was
            // a 120 ms SLEEP: rtlsdr_read_async returns while libusb still has cancelled
            // transfers outstanding, and the next synchronous control transfer (the
            // rtlsdr_reset_buffer on resume) runs libusb's event loop, which then completes
            // a transfer that has been freed. A sleep cannot be made correct, only longer —
            // and today it lost the race, with a full stack for the first time:
            //   usbi_handle_transfer_completion → … → rtlsdr_reset_buffer →
            //   resumeCaptureIdle → acceptWs   (SIGSEGV, 2026-07-28)
            // So the idle path now issues NO USB traffic at all: the stream keeps running
            // and asyncHandler drops the buffers. The DSP still parks on an empty queue,
            // which was always where most of the cost was.
            // ★ THE TRADE: the USB transfers themselves continue, so idle is no longer
            // ~0% — it is the cost of moving IQ we throw away. A crash that takes down an
            // unattended server is far worse than a little idle CPU. If that cost ever
            // matters more than it does now, the deterministic fix is close/reopen the
            // device, NOT a longer sleep.
            idleDiscard.store(true);
        }
        // ★★ LET LIBUSB FINISH REAPING THE CANCELLED TRANSFERS. rtlsdr_read_async can
        // return — and so the thread can join — while libusb still has cancelled transfers
        // outstanding. The next SYNCHRONOUS control transfer (rtlsdr_reset_buffer, on
        // resume) runs libusb's event loop, which then completes a transfer that has
        // already been freed, and libusb ASSERTS rather than returning an error. That is
        // an abort: uncatchable, and it takes the whole server down.
        // CRASHED ON EVERY CONNECT once the server had been idle (2026-07-26): the park
        // happens when the last listener leaves, so the fault is armed by an EMPTY server
        // and fired by the next person to arrive — the worst possible pairing for an
        // unattended machine, and invisible to whoever is testing with a client already
        // connected. Same shape as the AVAudioPlayerNode crash: an uncatchable abort on a
        // resume path, so it has to be prevented rather than handled.
        { std::lock_guard<std::mutex> lk(iqMtx); iqQueue.clear(); iqQueuedSamples = 0; iqPrefilled = false; }
        LOGI("no listeners — dongle capture paused (idle)");
    }
    void resumeCaptureIdle() {
        if (!captureIdle.exchange(false)) return;             // wasn't paused
        // ★★★ THIS `else` REPORTED A WORKING RADIO AS UNPLUGGED. With an Airspy attached, `dev`
        // is null, so the dongle branch called launchCapture() anyway — rtlsdr_read_async(NULL)
        // returned instantly, captureDown went true, and the watchdog then could not find an RTL
        // device and declared the receiver LOST. The Airspy had never stopped streaming, so the
        // result was audio playing happily under a banner saying "No radio connected to this
        // server" (Stuart, 2026-07-27, with a screenshot of exactly that).
        // ★ Same shape as radioCapsJson's `if (!useSdrplay())`: a two-source world expressed as
        // "the other one" quietly mis-handles the third. Name every source.
        if (useTcp()) { tcpRunning.store(true); rtlThread = std::thread([this]{ tcpReadLoop(); }); }
        else if (useSdrplay()) { sdrp->setPaused(false); }
        else if (useAirspyHf()) { ahf->setPaused(false); }
        else          { idleDiscard.store(false); }   // never stopped; just start wanting it again
        // ★★ AND RESET THE DSP. Restarting the capture alone leaves every recursive
        // state holding values from before the pause — filters, the pilot PLL, and the
        // RDS decoder's timing hypotheses, which is what actually broke: after an idle
        // period RDS came back HALF working (groups, AF and PTY fine; PI, station name
        // and RadioText never appearing) at 61 dB SNR, and a retune could not clear it
        // because a retune rebuilds the channel, not the decoder (Stuart, 2026-07-28).
        rx.requestReset();
        LOGI("listener connected — dongle capture resumed, DSP state reset");
    }

    /** Advanced RDS: the fields the normal path discards, plus the constellation.
     *  Points are sent as signed bytes — the plot is 64 dots in a small box, so a float
     *  would be fifty times the bytes for precision no eye can resolve. */
    void sendRdsExt(std::shared_ptr<net::Socket> sock) {
        if (!sock || !sock->isOpen()) return;
        int pty, tp, ta, ms, di, ctMin, ctOff, gTot, afSeen;
        int ptyR, tpR, taR, msR, diR;
        int lang, pinD, pinH, pinM; float phase, phaseCoh, pilotDev, rdsDev_, phaseDrift;
        int berNow;   // ★ block error rate, ALSO here: the phase verdict needs it
        std::string rtpT, rtpA, lps, ptyn;
        std::vector<vibedsp::RdsDecoder::Eon> eon;
        std::vector<vibedsp::RdsDecoder::Oda> oda;
        std::vector<int> af, grp; std::vector<float> pts, mpx;
        { std::lock_guard<std::mutex> lk(rdsMtx);
          pty = rdsPty; tp = rdsTp; ta = rdsTa; ms = rdsMs; di = rdsDi;
          ptyR = rdsPtyRaw; tpR = rdsTpRaw; taR = rdsTaRaw; msR = rdsMsRaw; diR = rdsDiRaw;
          ctMin = rdsCtMin; ctOff = rdsCtOff; gTot = rdsGrpTotal;
          af = rdsAf; grp = rdsGrp; pts = rdsConst; mpx = rdsMpx; afSeen = rdsAfSeen;
          rtpT = rdsRtpTitle; rtpA = rdsRtpArtist; lps = rdsLongPs; ptyn = rdsPtyn;
          lang = rdsLang; pinD = rdsPinDay; pinH = rdsPinHour; pinM = rdsPinMin;
          eon = rdsEon; oda = rdsOda; phase = rdsPhase; phaseCoh = rdsPhaseCoh;
          phaseDrift = rdsPhaseDrift;
          pilotDev = rdsPilotDev; rdsDev_ = rdsDev; berNow = rdsBer; }
        std::string j = "{\"type\":\"rdsx\",\"pty\":" + std::to_string(pty)
                      + ",\"tp\":"  + std::to_string(tp)
                      + ",\"ta\":"  + std::to_string(ta)
                      + ",\"ms\":"  + std::to_string(ms)
                      + ",\"di\":"  + std::to_string(di)
                      // ★ RAW alongside CONFIRMED, always both. The client picks; the server
                      // never changes behaviour for one listener on a shared receiver.
                      + ",\"ptyRaw\":" + std::to_string(ptyR)
                      + ",\"tpRaw\":"  + std::to_string(tpR)
                      + ",\"taRaw\":"  + std::to_string(taR)
                      + ",\"msRaw\":"  + std::to_string(msR)
                      + ",\"diRaw\":"  + std::to_string(diR)
                      + ",\"ct\":"  + std::to_string(ctMin)
                      + ",\"ctoff\":" + std::to_string(ctOff)
                      + ",\"gtot\":"  + std::to_string(gTot)
                      + ",\"afseen\":" + std::to_string(afSeen)
                      + ",\"rtpTitle\":\"" + jsonEscape(rtpT) + "\""
                      + ",\"rtpArtist\":\"" + jsonEscape(rtpA) + "\""
                      + ",\"longPs\":\"" + jsonEscape(lps) + "\""
                      + ",\"ptyn\":\"" + jsonEscape(ptyn) + "\""
                      + ",\"lang\":" + std::to_string(lang)
                      + ",\"pinDay\":" + std::to_string(pinD)
                      + ",\"pinHour\":" + std::to_string(pinH)
                      + ",\"pinMin\":" + std::to_string(pinM)
                      + ",\"phase\":" + std::to_string(phase)
                      + ",\"phaseDrift\":" + std::to_string(phaseDrift)
                      + ",\"phaseCoh\":" + std::to_string(phaseCoh)
                      + ",\"pilotDev\":" + std::to_string(pilotDev)
                      + ",\"rdsDev\":" + std::to_string(rdsDev_)
                      + ",\"ber\":" + std::to_string(berNow)
                      + ",\"grp\":[";
        for (size_t i = 0; i < grp.size(); ++i) { if (i) j += ','; j += std::to_string(grp[i]); }
        j += "],\"eon\":[";
        for (size_t i = 0; i < eon.size(); ++i) {
            if (i) j += ',';
            char pib[8]; snprintf(pib, sizeof pib, "%04X", eon[i].pi);
            j += "{\"pi\":\"" + std::string(pib) + "\",\"ps\":\""
               + jsonEscape(std::string(eon[i].ps, strnlen(eon[i].ps, 8))) + "\",\"af\":"
               + std::to_string(eon[i].afKhz) + ",\"ta\":" + std::to_string(eon[i].ta) + "}";
        }
        j += "],\"oda\":[";
        for (size_t i = 0; i < oda.size(); ++i) {
            if (i) j += ',';
            char ab[8]; snprintf(ab, sizeof ab, "%04X", oda[i].aid);
            j += "{\"aid\":\"" + std::string(ab) + "\",\"grp\":"
               + std::to_string(oda[i].group) + "}";
        }
        j += "],\"af\":[";
        for (size_t i = 0; i < af.size(); ++i) {
            if (i) j += ',';
            j += std::to_string(af[i]);
        }
        j += "],\"xy\":[";
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) j += ',';
            float v = pts[i] * 100.0f;
            if (v > 127.0f) v = 127.0f; else if (v < -127.0f) v = -127.0f;
            j += std::to_string((int)v);
        }
        // ★ MPX as signed bytes: the plot is a small strip, so a dB value to the nearest
        // decibel is finer than any pixel can show and a tenth of the bytes of a float.
        j += "],\"mpx\":[";
        for (size_t i = 0; i < mpx.size(); ++i) {
            if (i) j += ',';
            int v = (int)lround(mpx[i]);
            if (v < -128) v = -128; else if (v > 0) v = 0;
            j += std::to_string(v);
        }
        j += "]}";
        sendText(sock, j);
    }

    /** ★★ THE SESSION TIME LIMIT. Warn, then evict, then hold the address on cooldown.
     *
     *  ★ EXEMPTIONS ARE NOT POLITENESS, THEY ARE CORRECTNESS. Loopback is the host listening on
     *  their own machine — they are not queueing for anything, and the limit rations a queue.
     *  An admin session is the owner, who must not be able to lock themselves out of their own
     *  receiver from across the house.
     *
     *  ★ WARN BEFORE ENDING IT. A session that simply stops reads as a crash, and the listener
     *  blames the software rather than understanding they had a share of a shared radio. */
    void enforceSessionLimit() {
        const int limitMin = g_vsSessionLimitMin.load();
        if (limitMin <= 0) return;                       // unlimited: the default
        if (adminOk.load()) return;                      // the owner is exempt

        std::shared_ptr<net::Socket> spec, aud;
        std::string addr; double since; int warned;
        { std::lock_guard<std::mutex> lk(clientMtx);
          if (occupantSession.empty() || occupantSince <= 0) return;
          spec = specClient; aud = audioClient;
          addr = occupantAddr; since = occupantSince; warned = occupantWarned; }
        if (addr.empty() || isLoopback(addr)) return;    // the host's own listening

        const double elapsed = Impl::nowSecs() - since;
        const double left    = (double)limitMin * 60.0 - elapsed;

        if (left > 0) {
            // Two warnings, each once: enough notice to finish listening to something, then a
            // final one. Sent to the spectrum socket, which every client holds.
            const int stage = left <= 30 ? 2 : left <= 120 ? 1 : 0;
            if (stage > 0 && !(warned & stage)) {
                { std::lock_guard<std::mutex> lk(clientMtx); occupantWarned |= stage; }
                const std::string m = "{\"type\":\"session_warning\",\"secs\":"
                                    + std::to_string((int)(left + 0.5)) + "}";
                if (spec && spec->isOpen()) sendWs(spec, 0x1, (const uint8_t*)m.data(), m.size());
            }
            return;
        }

        LOGI("session limit reached (%d min) — ending %s", limitMin, addr.c_str());
        const std::string m = "{\"type\":\"session_expired\",\"cooldown\":"
                            + std::to_string(kSessionCooldownSec) + "}";
        // ★ TELL THEM FIRST, THEN CLOSE. The message is what stops the client treating this as
        // a dropped link and retry-storming a server that is deliberately turning it away.
        if (spec && spec->isOpen()) { sendWs(spec, 0x1, (const uint8_t*)m.data(), m.size()); spec->close(); }
        if (aud  && aud->isOpen())  { aud->close(); }
        { std::lock_guard<std::mutex> lk(clientMtx);
          cooldownUntil[addr] = Impl::nowSecs() + kSessionCooldownSec;
          occupantSession.clear(); occupantSince = 0; occupantWarned = 0; occupantAddr.clear(); }
    }

    /** Tell every connected client whether we currently have a radio. They draw the message. */
    void notifyDeviceState() {
        std::shared_ptr<net::Socket> sock;
        { std::lock_guard<std::mutex> lk(clientMtx); sock = specClient; }
        if (!sock || !sock->isOpen()) return;
        sendText(sock, deviceLost.load()
            ? "{\"type\":\"device\",\"present\":false}"
            : "{\"type\":\"device\",\"present\":true}");
    }

    /** Re-find our dongle after a replug. Prefer the SERIAL — indices renumber when a different
     *  dongle is unplugged, and reopening by index can hand the listener a different receiver. */
    int findOurDevice() const {
        const uint32_t n = rtlsdr_get_device_count();
        if (n == 0) return -1;
        if (!usbSerial.empty()) {
            for (uint32_t i = 0; i < n; i++) {
                char mfr[256] = {0}, prd[256] = {0}, ser[256] = {0};
                if (rtlsdr_get_device_usb_strings(i, mfr, prd, ser) == 0 && usbSerial == ser)
                    return (int)i;
            }
            return -1;      // ours specifically is not back yet — do NOT grab someone else's
        }
        return (usbIndex >= 0 && (uint32_t)usbIndex < n) ? usbIndex : 0;
    }

    /**
     * Reopen and restart capture. ★ NOT CALLED — kept as the skeleton of the real fix.
     *
     * Calling this from the watchdog thread CRASHED the server on replug: it closes and reopens
     * `dev` while the HTTP/control threads are still calling rtlsdr_set_gain / tuneHw / setFftRate
     * on that same pointer, with nothing serialising them. Before this can be used, every
     * rtlsdr_* call site must go behind one device mutex — a real refactor, and it must be done
     * with a test that hammers control calls while unplugging.
     */
    bool reopenDevice() {
        const int idx = findOurDevice();
        if (idx < 0) return false;

        if (rtlThread.joinable()) rtlThread.join();     // the old capture thread has exited
        if (dev) { rtlsdr_close(dev); dev = nullptr; }

        if (rtlsdr_open(&dev, (uint32_t)idx) != 0 || !dev) { dev = nullptr; return false; }

        // Re-apply everything the device forgot by being unplugged. Same order as start().
        rtlsdr_set_sample_rate(dev, (uint32_t)sampleRate);
        tuneHw(rtlCenter.load());
        if (lastGainTenthDb < 0) rtlsdr_set_tuner_gain_mode(dev, 0);
        else { rtlsdr_set_tuner_gain_mode(dev, 1); rtlsdr_set_tuner_gain(dev, lastGainTenthDb); }
        rtlsdr_reset_buffer(dev);

        launchCapture();
        const bool wasLost = deviceLost.exchange(false);
        captureDown.store(false);
        (void)wasLost;
        LOGI("RTL-SDR back — capture resumed on device %d", idx);
        notifyDeviceState();
        return true;
    }

    void startHotplugWatch() {
        if (hotplugRun.exchange(true)) return;
        hotplugThread = std::thread([this]{
            vibeThreadName("vibe-hotplug");
            while (hotplugRun.load()) {
                // 2s: fast enough that a replug feels immediate, slow enough that scanning USB
                // costs nothing measurable on a Pi.
                for (int i = 0; i < 20 && hotplugRun.load(); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!hotplugRun.load()) break;
                if (stopping.load() || restarting.load()) continue;

                // ★ SILENCE = GONE. 3s is far longer than any legitimate gap (a rate change is
                // flagged by `restarting`, and normal delivery is continuous), and short enough
                // that the message arrives while the user is still holding the plug.
                double last = lastIqAt.load(std::memory_order_relaxed);
                // ★★ ASK THE RADIO, NOT THE PIPELINE. `lastIqAt` is stamped by the SINK, so
                // it stops advancing whenever we idle-park and throw samples away — which made a
                // parked Airspy look exactly like an unplugged one. The source knows when the
                // hardware last delivered, whatever we then did with it.
                if (useAirspyHf()) last = std::max(last, ahf->lastRxSecs());
                const bool silent = last > 0 && (nowSecs() - last) > 3.0;

                // ★★★ AN RSP STALL IS RECOVERABLE IN PLACE — and unlike a dongle, nothing has
                // been unplugged. The SDRplay API can simply stop calling the stream callback
                // while every handle stays valid and every call keeps returning Success: audio
                // and spectrum freeze, the server still reports itself serving, and only
                // killing the process brought it back (Stuart, 2026-07-27). Re-Initing the
                // stream fixes it without disturbing the listener's session.
                // ★ The dongle path deliberately does NOT reopen (see below) because `dev` is
                // touched unlocked from the control threads. That objection does not apply
                // here: restartStream() takes SdrplaySource's own lock, which every API-
                // touching call on the object now also takes.
                // ★ BACK OFF. If the API is wedged rather than merely stalled, retrying every
                // 2 s forever would hammer a system-wide mutex and bury the real error; after
                // a few goes, stop and report honestly rather than thrash.
                // ★★★ NEVER GIVE UP FOR GOOD. This was `if (sdrpRestarts < 4)` with the counter
                // reset ONLY on a healthy tick — and a tick can only be healthy if a restart
                // already worked. So four consecutive failures wedged the counter at 4 and the
                // radio was never retried again for the life of the process: the operator saw a
                // dead receiver that only a full restart cured (Stuart, on the Mac with an RSP,
                // 2026-07-31). Back OFF instead of stopping: attempt n waits n*2s (capped at
                // 30s) before the next go, so a genuinely wedged API isn't hammered and a radio
                // that recovers on its own — a re-plug, a resumed hub — is picked straight back
                // up with nobody having to touch anything.
                const bool recoverable = silent && !captureIdle.load() &&
                                         ((useSdrplay() && sdrp) || (useAirspyHf() && ahf));
                if (recoverable) {
                    const double waitS = std::min(30.0, 2.0 * (double)(srcRestarts + 1));
                    if (nowSecs() - lastRestartAt >= waitS) {
                        ++srcRestarts;
                        lastRestartAt = nowSecs();
                        std::string rerr;
                        bool ok = false;
                        if (useSdrplay() && sdrp) {
                            // ★★★ ESCALATE, exactly as the Airspy does below. Re-Init cures a
                            // STALL — the API quietly stopping the callback with every handle
                            // still valid — and that is the commoner fault, so it goes first.
                            // It cannot cure a NUDGED PLUG: a re-enumerated device invalidates
                            // the selected handle, so Uninit + Init on it fails forever and the
                            // operator is left stopping and starting the whole server (Stuart,
                            // 2026-08-02). After two goes the handle itself is the suspect, so
                            // release the device and select it again from a fresh enumeration.
                            const bool deep = srcRestarts > 2;
                            LOGE("no IQ for 3s on an RSP — %s (attempt %d)",
                                 deep ? "reopening the device" : "re-initialising the stream",
                                 srcRestarts);
                            ok = deep ? sdrp->reopen(rerr) : sdrp->restartStream(rerr);
                        } else {
                            // ★ ESCALATE. The first two goes restart the stream on the handle we
                            // hold, which is all a stalled-but-present radio needs. After that the
                            // handle itself is the suspect, so close and reopen by serial — the
                            // case where a nudged USB plug left the device enumerated (LEDs still
                            // lit) but the handle dead.
                            const bool deep = srcRestarts > 2;
                            LOGE("no IQ for 3s on an Airspy HF+ — %s (attempt %d)",
                                 deep ? "reopening the device" : "restarting the stream",
                                 srcRestarts);
                            ok = ahf->restartStream(deep, rerr);
                        }
                        if (ok) {
                            // Give it a fresh clock, or the next tick sees the OLD timestamp
                            // and declares another stall before any sample could have arrived.
                            lastIqAt.store(nowSecs(), std::memory_order_relaxed);
                            continue;
                        }
                        LOGE("stream restart failed: %s", rerr.c_str());
                    }
                    // ★★★ FALL THROUGH TO THE REPORTING BELOW — do NOT `continue` while waiting
                    // out the back-off. An earlier draft of this did, and it recreated the exact
                    // bug this commit exists to kill: with the back-off widening to 30 s, a radio
                    // that never recovers would sit un-reported indefinitely and the UI would show
                    // a healthy receiver over silence. Retrying and telling the truth meanwhile
                    // are not alternatives.
                } else if (!silent) {
                    // Healthy again: the next stall gets a full set of tries from scratch.
                    srcRestarts = 0;
                    lastRestartAt = 0.0;
                }

                if (silent && !deviceLost.load()) {
                    deviceLost.store(true);
                    captureDown.store(true);
                    LOGE("no IQ for 3s — dongle unplugged or failed");
                    notifyDeviceState();
                }
                if (!captureDown.load()) continue;
                // ★ DELIBERATELY DOES NOT REOPEN. The first version did, from THIS thread, while
                // the HTTP/control threads were still calling rtlsdr_set_gain / tuneHw on the same
                // `dev` pointer with no lock — closing and reopening underneath them crashed the
                // whole server on replug, which is far worse than the black screen it was meant to
                // cure. Safe in-process recovery needs `dev` behind a mutex that every rtlsdr_*
                // call site respects; that is a real refactor, not a late-night patch.
                // For now: report the truth and let the operator restart.
                // ★ "IS IT BACK?" IS PER-SOURCE. findOurDevice() enumerates DONGLES, so asking
                // it about an Airspy or an RSP always answers no — which would report a
                // perfectly present radio as gone the moment this line was ever reached.
                const bool back = useAirspyHf() ? (vibe::AirspyHfSource::deviceCount() > 0)
                                : useSdrplay()  ? (vibe::SdrplaySource::deviceCount() > 0)
                                                : (findOurDevice() >= 0);
                if (back == deviceLost.load()) {      // state changed
                    deviceLost.store(!back);
                    notifyDeviceState();
                }
            }
        });
    }

    void stopHotplugWatch() {
        if (!hotplugRun.exchange(false)) return;
        if (hotplugThread.joinable()) hotplugThread.join();
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
        Impl* self = (Impl*)ctx;
        // ★ Stamp FIRST, even while discarding: the watchdog measures "is IQ still
        // arriving", and it is — we are simply choosing not to want it. Skipping the
        // stamp would make an idle server look like a dead dongle.
        self->lastIqAt.store(nowSecs(), std::memory_order_relaxed);
        if (self->idleDiscard.load(std::memory_order_relaxed)) return;   // nobody listening
        self->enqueueIq(buf, (int)(len / 2));
    }

    static double nowSecs() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
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

// ★★★ THE LISTENER'S DSP CHOICES, HELD OUTSIDE `p` SO THEY SURVIVE A RADIO RESTART.
//
// `p` is a raw Impl* that FIVE separate start paths replace with a fresh `new Impl()`, and
// every one of those resets these fields to their constructor defaults. So anything the
// client had chosen was silently reverted by a rate change, a USB nudge, an idle resume or
// simply being the first client to arrive before the radio was up — while the client's own
// UI went on displaying the choice it had made. A setter called while `p` was null did not
// even get that far: `if (!p) return` dropped it on the floor.
//
// ★ De-emphasis is where it was NOTICED, because it is the one setting you can HEAR being
// wrong: HansVanEijsden reported the panel reading OFF over obviously de-emphasised audio,
// and "a quick settings change fixes it" — the change re-sent the value to a live Impl
// (Stuart had seen it before too, 2026-07-27). Squelch, NR, notch and stereo had the same
// bug and no such tell; NR and notch silently OFF just sound like a slightly worse receiver.
//
// ★ Defaults MUST match Impl's own field initialisers, or applying this record at startup
// would itself change the radio. Kept next to each other deliberately.
struct DesiredDsp {
    std::atomic<double> deempTau{50e-6};    // Impl::deempTau
    std::atomic<bool>   squelchOn{false};   // Impl::squelchOn
    std::atomic<float>  squelchDb{-50.0f};  // Impl::squelchDb
    std::atomic<bool>   nrOn{false};        // Impl::nrOn
    std::atomic<float>  nrStrength{-1.0f};  // <0 = never set, so leave the engine alone
    std::atomic<bool>   notchOn{false};     // Impl::notchOn
    std::atomic<bool>   stereoOn{true};     // RxPipeline defaults to stereo enabled
    // ★★ THE RSP CONTROLS TOO — same bug, separately reported: "RF gain on RSP1B not
    // remembered between sessions" (Stuart, 2026-07-27). The web client was innocent; it
    // saves every one of these and re-pushes them when the radio announces itself. They were
    // lost at the OTHER end, in setters shaped `if (p && p->useSdrplay())` with nothing to
    // replay them onto the fresh Impl a restart creates.
    // ★ SENTINELS, not defaults: -1 / -999 mean "the listener never chose one", so a restart
    // re-applies only what was actually set and never overwrites the API's own starting point
    // with a value we invented.
    std::atomic<int>  rspLna{-1};
    std::atomic<int>  rspIfGr{-1};
    std::atomic<int>  rspAgcSet{-999};
    std::atomic<int>  rspIfAgc{-1};      // tri-state: -1 unset, 0 off, 1 on
    std::atomic<int>  rspRfNotch{-1};
    std::atomic<int>  rspDabNotch{-1};
    // ★ Airspy HF+ controls, held here for exactly the reason the RSP ones are: five start
    // paths each build a fresh Impl, and a setter that only writes through `p` is lost the
    // moment one of them runs. Same sentinels — -1 means "the listener never chose".
    std::atomic<int>  ahfAgc{-1};        // tri-state: -1 unset, 0 off, 1 on
    std::atomic<int>  ahfAgcHigh{-1};
    std::atomic<int>  ahfAtt{-1};        // 0..8, 6 dB steps
    std::atomic<int>  ahfLna{-1};
    std::atomic<int>  ahfPpb{INT32_MIN}; // calibration; INT32_MIN = never set
};
static DesiredDsp g_dsp;

// Replay the listener's choices onto a freshly built Impl. ★ Call this at EVERY `p = impl`
// site — there are five, one per source type, and a new one that forgets to call it
// reintroduces exactly the bug this exists to kill.
void LocalSdrShim::applyDesiredDsp(LocalSdrShim::Impl* impl) {
    if (!impl) return;
    impl->squelchOn.store(g_dsp.squelchOn.load());
    impl->squelchDb.store(g_dsp.squelchDb.load());
    impl->nrOn.store(g_dsp.nrOn.load());
    impl->notchOn.store(g_dsp.notchOn.load());
    impl->rx.setStereoEnabled(g_dsp.stereoOn.load());
    impl->deempTau = g_dsp.deempTau.load();
    impl->rx.setDeemphasis(impl->deempTau);
    // ★ RSP controls, only when this radio IS an RSP and only what was actually chosen.
    if (impl->useSdrplay() && impl->sdrp) {
        if (g_dsp.rspLna.load()      >= 0)    impl->sdrp->setLnaState(g_dsp.rspLna.load());
        if (g_dsp.rspAgcSet.load()   > -999)  impl->sdrp->setIfAgcSetPoint(g_dsp.rspAgcSet.load());
        if (g_dsp.rspRfNotch.load()  >= 0)    impl->sdrp->setRfNotch(g_dsp.rspRfNotch.load() != 0);
        if (g_dsp.rspDabNotch.load() >= 0)    impl->sdrp->setDabNotch(g_dsp.rspDabNotch.load() != 0);
        // ★ ORDER MATTERS, exactly as it does in the client's pushAllRspSettings: a manual IF
        // reduction is refused while the AGC owns that register, so set the AGC state FIRST and
        // only push a manual IFGR when the AGC is off. Reversing these drops the value silently.
        const int agc = g_dsp.rspIfAgc.load();
        if (agc >= 0) impl->sdrp->setIfAgc(agc != 0);
        if (agc == 0 && g_dsp.rspIfGr.load() >= 0)
            impl->sdrp->setIfGainReduction(g_dsp.rspIfGr.load());
    }
    if (impl->useAirspyHf() && impl->ahf) {
        if (g_dsp.ahfAtt.load()     >= 0) impl->ahf->setAttenuation(g_dsp.ahfAtt.load());
        if (g_dsp.ahfLna.load()     >= 0) impl->ahf->setLna(g_dsp.ahfLna.load() != 0);
        if (g_dsp.ahfAgcHigh.load() >= 0) impl->ahf->setAgcThreshold(g_dsp.ahfAgcHigh.load() != 0);
        // ★ AGC LAST, as with the RSP: it owns the gain path, so setting it after the manual
        // controls is what makes "AGC off + a chosen attenuation" land in that order.
        if (g_dsp.ahfAgc.load()     >= 0) impl->ahf->setAgc(g_dsp.ahfAgc.load() != 0);
        if (g_dsp.ahfPpb.load() != INT32_MIN) impl->ahf->setCalibrationPpb(g_dsp.ahfPpb.load());
    }
    const float nrs = g_dsp.nrStrength.load();
    if (nrs >= 0.0f) {   // only if the client ever set one; else leave the engine's own
        std::lock_guard<std::mutex> lk(impl->nrMtx);
        if (!impl->nrEng) impl->nrEng = new AudioNR();
        impl->nrEng->setStrength(nrs);
    }
}

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
void LocalSdrShim::setVibeServerPort(int port) {
    g_vsPort.store(port > 0 ? port : 0);
    LOGI("VibeServer port: %s", port > 0 ? std::to_string(port).c_str() : "auto (48000-48049)");
}
void LocalSdrShim::setVibeServerAuth(const std::string& secret) {
    std::lock_guard<std::mutex> lk(g_vsMtx); g_vsSecret = secret;
}
void LocalSdrShim::summonClient() {
    // "The person at the host is looking for you." Costs nothing when nobody is listening.
    if (!p) return;
    std::shared_ptr<net::Socket> sock;
    { std::lock_guard<std::mutex> lk(p->clientMtx); sock = p->specClient; }
    if (sock && sock->isOpen()) p->sendText(sock, "{\"type\":\"summon\"}");
}
void LocalSdrShim::setVibeServerForceIdleSaver(bool on) {
    g_vsForceIdle.store(on);
    LOGI("VibeServer idle saver: %s", on ? "REQUIRED (clients may not disable)" : "listener's choice");
}
void LocalSdrShim::setVibeServerLimits(double maxBandwidthHz, double maxFftRate) {
    g_vsMaxBandwidth.store(maxBandwidthHz); g_vsMaxFftRate.store(maxFftRate);
}
void LocalSdrShim::setVibeServerCompressAudio(bool on) { g_vsCompressAudio.store(on); }
void LocalSdrShim::setVibeServerUncompressedAudio(int mode) { g_vsUncompressedAudio.store(mode); }
void LocalSdrShim::setVibeServerAdminSecret(const std::string& secret) {
    std::lock_guard<std::mutex> lk(g_vsAdminMtx);
    g_vsAdminSecret = secret;
    LOGI("admin secret set (%zu chars)", secret.size());
}


void LocalSdrShim::setVibeServerSessionLimit(int minutes) {
    g_vsSessionLimitMin.store(minutes > 0 ? minutes : 0);
    LOGI("session limit set to %d min", g_vsSessionLimitMin.load());
}

bool LocalSdrShim::isBusy() const {
    if (!p) return false;
    std::lock_guard<std::mutex> lk(p->clientMtx);
    return !p->occupantSession.empty()
        && ((p->specClient && p->specClient->isOpen())
         || (p->audioClient && p->audioClient->isOpen()));
}

int LocalSdrShim::occupantSecsLeft() const {
    const int limitMin = g_vsSessionLimitMin.load();
    if (!p || limitMin <= 0) return -1;
    if (p->adminOk.load()) return -1;                    // owner: exempt, so no countdown
    std::lock_guard<std::mutex> lk(p->clientMtx);
    if (p->occupantSession.empty() || p->occupantSince <= 0) return -1;
    if (p->occupantAddr.empty() || isLoopback(p->occupantAddr)) return -1;
    const double left = (double)limitMin * 60.0 - (Impl::nowSecs() - p->occupantSince);
    return left > 0 ? (int)(left + 0.5) : 0;
}

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
    // ★★ AN AIRSPY HF+ ARRIVES DOWN THE SAME PIPE. Android hands us an fd together with the
    // VID/PID it came from, so the radio can be identified HERE — no second JNI entry point, no
    // parallel Kotlin path, and one place that decides which driver a descriptor belongs to.
    if (vid == 0x03eb && pid == 0x800c) {
        return startAirspyHfFd(fd, centerFreq, sampleRate, gainTenthDb,
                               fftSize, fftRate, mode, err);
    }
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

    // TWO WAYS IN, because the platforms differ in who is allowed to claim the USB device.
    //
    //   fd >= 0 — ANDROID. Java owns the device (the app has no permission to claim it from
    //             native), opens it, and hands us a descriptor to wrap. We dup it so the
    //             caller keeps ownership of theirs.
    //   fd <  0 — DESKTOP (macOS/Linux). libusb claims the dongle directly, so `fd` carries a
    //             DEVICE INDEX instead (negated: -1 = index 0, -2 = index 1, …). No helper
    //             process, no rtl_tcp — this is what makes a standalone desktop app possible.
    int ret;
#ifdef __ANDROID__
    // rtlsdr_open_sys_dev() is an ANDROID-ONLY librtlsdr extension (upstream has no such call),
    // so the fd path cannot even be compiled on desktop — and is never taken there anyway.
    if (fd >= 0) {
        impl->usbFd = dup(fd);
        if (impl->usbFd < 0) { err = "dup(usb fd) failed"; delete impl; return -1; }
        ret = rtlsdr_open_sys_dev(&impl->dev, (intptr_t)impl->usbFd);
        if (ret != 0 || !impl->dev) { err = "rtlsdr_open_sys_dev failed: " + std::to_string(ret); ::close(impl->usbFd); delete impl; return -1; }
    } else
#endif
    {
        const int index = -fd - 1;
        const uint32_t nDev = rtlsdr_get_device_count();
        if (nDev == 0) { err = "no SDR found — is it plugged in?"; delete impl; return -1; }
        if ((uint32_t)index >= nDev) {
            err = "RTL-SDR index " + std::to_string(index) + " out of range (" + std::to_string(nDev) + " found)";
            delete impl; return -1;
        }
        impl->usbFd = -1;   // we own the device via librtlsdr, there is no fd of ours to close
        ret = rtlsdr_open(&impl->dev, (uint32_t)index);
        if (ret != 0 || !impl->dev) {
            err = "rtlsdr_open(" + std::to_string(index) + ") failed: " + std::to_string(ret)
                + " — is another program using the dongle?";
            delete impl; return -1;
        }
        LOGI("USB device %d opened: %s", index, rtlsdr_get_device_name((uint32_t)index));
        // Keep BOTH: the serial is how we find this same dongle again after a replug (indices
        // renumber when another one is unplugged); the index is the fallback when it has no serial.
        impl->usbIndex = index;
        { char mfr[256] = {0}, prd[256] = {0}, ser[256] = {0};
          if (rtlsdr_get_device_usb_strings((uint32_t)index, mfr, prd, ser) == 0) impl->usbSerial = ser; }
    }
    rtlsdr_set_sample_rate(impl->dev, (uint32_t)sampleRate);
    // Offset tuning: physically tune HW_OFFSET_HZ above the logical centre.
    impl->tuneHw(centerFreq);
    impl->lastGainTenthDb = gainTenthDb;   // re-applied if the dongle is replugged
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
    if (int want = g_vsPort.load(); want > 0) {
        try { impl->listener = net::listen(bindHost(), want); chosen = want; }
        catch (...) { impl->listener = nullptr; }
    } else {
        for (int port = 48000; port < 48050; port++) {
            try { impl->listener = net::listen(bindHost(), port); chosen = port; break; }
            catch (...) { impl->listener = nullptr; }
        }
    }
    if (!impl->listener) {
        err = g_vsPort.load() > 0
            ? "port " + std::to_string(g_vsPort.load()) + " is already in use — choose another"
            : "no free port in 48000-48049";
        impl->teardownAudio(); impl->rx.stop(); rtlsdr_close(impl->dev);
        if (impl->usbFd >= 0) ::close(impl->usbFd);   // -1 on the desktop path (no fd of ours)
        delete impl; return -1;
    }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });

    impl->startDspThread();
    {
        const uint32_t bufLen = rtlBufLenForRate(impl->sampleRate);
        (void)bufLen;
        impl->launchCapture();
        impl->startHotplugWatch();
    }

    p = impl;
    LocalSdrShim::applyDesiredDsp(impl);   // the listener's DSP choices survive this restart
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


/**
 * Start on an SDRplay RSP.
 *
 * ★ Mirrors startTcp deliberately rather than generalising the three sources into one
 * abstraction: the shim has 56 direct librtlsdr calls and refactoring all of them tonight
 * would risk the path that actually works for the sake of tidiness. A third sibling is the
 * honest small change; the abstraction can come when something needs it.
 *
 * ★ The samples land in enqueueIqInt16 — the SAME queue the SpyServer path already fills,
 * which is why 14-bit hardware needed no new DSP: the shim has handled int16 IQ all along.
 */
// ★ Mirrors startSdrplay closely on purpose — same lifecycle, same ordering, same watchdog.
// The differences are all in the source object: complex float instead of int16, a rate list
// enumerated from the device, and a tuning range with a hole in it.
// ★ Same as startAirspyHf() but from a USB descriptor. Only the acquisition differs, so the
// two share everything through a common tail rather than drifting apart.
int LocalSdrShim::startAirspyHfFd(int fd,
                                  double centerFreq, double sampleRate, int gainTenthDb,
                                  int fftSize, double fftRate, const std::string& mode,
                                  std::string& err) {
    return startAirspyHfCommon(-1, fd, centerFreq, sampleRate, gainTenthDb,
                               fftSize, fftRate, mode, err);
}

int LocalSdrShim::startAirspyHf(int index,
                                double centerFreq, double sampleRate, int gainTenthDb,
                                int fftSize, double fftRate, const std::string& mode,
                                std::string& err) {
    return startAirspyHfCommon(index, -1, centerFreq, sampleRate, gainTenthDb,
                               fftSize, fftRate, mode, err);
}

int LocalSdrShim::startAirspyHfCommon(int index, int fd,
                                      double centerFreq, double sampleRate, int gainTenthDb,
                                      int fftSize, double fftRate, const std::string& mode,
                                      std::string& err) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    if (p) { LOGI("stale shim found on Airspy HF+ start — tearing down"); stopLocked(); }
    auto* impl = new Impl();
    impl->fftRate = fftRate;
    impl->rtlCenter.store(centerFreq);
    impl->viewCenter.store(centerFreq);
    impl->audioFreq.store(centerFreq);
    impl->mode = mode.empty() ? "wfm" : mode;
    impl->lastGainTenthDb = gainTenthDb;

    // ★★★ THE HF+ ALWAYS OPENS AT ITS OWN DEFAULT RATE — the caller does not get a say, and
    // neither does the server's config. The picker went in 1c2c88e because re-rating a LIVE HF+
    // wedges it; this closes the other half, where an owner sets an unusual rate in the config
    // file and the radio comes up in a state nothing else is built for. Two things break at any
    // other rate: the dead-lobe crop is a per-rate table only MEASURED at the top rate, and
    // 228 kHz tunes ~7.8 kHz off frequency on the current firmware (open, unexplained).
    // ★ Stuart: "I think we just offer the default on the server too otherwise that breaks all
    // the dead space fix we added." 0 = "ask the radio" — see nearestRate().
    sampleRate = 0.0;
    impl->ahf = std::make_unique<vibe::AirspyHfSource>();
    Impl* self = impl;
    impl->ahf->setSink([self](const float* iq, int n) {
        self->lastIqAt.store(Impl::nowSecs(), std::memory_order_relaxed);
        self->enqueueIqFloat(iq, n, /*blockIfFull=*/false);
    });
    const bool opened = (fd >= 0)
        ? impl->ahf->openFd(fd, sampleRate, centerFreq, gainTenthDb, err)
        : impl->ahf->open(index, sampleRate, centerFreq, gainTenthDb, err);
    if (!opened) { delete impl; return -1; }
    // ★ THE RADIO DECIDES THE RATE, not the caller. An HF+ Discovery tops out near 768 kHz
    // where a dongle does 2.4 MSPS, so a saved preference from a previous radio would ask for
    // something impossible — open() has already snapped it to the nearest real one, and
    // everything downstream (FFT size, channel decimation) must be built from THAT, not from
    // what was requested.
    const auto& rl = impl->ahf->sampleRates();
    impl->sampleRate = rl.empty() ? sampleRate : (double)impl->ahf->nearestRate(sampleRate);
    impl->fftSize = fftSizeForRate(impl->sampleRate);
    impl->startEngine();
    impl->buildAudio();

    if (!impl->ahf->start(err)) {
        impl->teardownAudio(); impl->rx.stop();
        impl->ahf->close(); delete impl; return -1;
    }

    int chosen = -1;
    if (int want = g_vsPort.load(); want > 0) {
        try { impl->listener = net::listen(bindHost(), want); chosen = want; }
        catch (...) { impl->listener = nullptr; }
    } else {
        for (int p2 = 48000; p2 < 48050; p2++) {
            try { impl->listener = net::listen(bindHost(), p2); chosen = p2; break; }
            catch (...) { impl->listener = nullptr; }
        }
    }
    if (!impl->listener) {
        err = g_vsPort.load() > 0
            ? "port " + std::to_string(g_vsPort.load()) + " is already in use — choose another"
            : "no free port in 48000-48049";
        impl->teardownAudio(); impl->rx.stop();
        impl->ahf->close(); delete impl; return -1;
    }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });
    impl->startDspThread();

    p = impl;
    LocalSdrShim::applyDesiredDsp(impl);
    impl->startHotplugWatch();   // same silence watchdog as every other source
    LOGI("Airspy HF+ started: index=%d center=%.0f rate=%.0f port=%d",
         index, centerFreq, impl->sampleRate, chosen);
    return chosen;
}

int LocalSdrShim::startSdrplay(int index,
                               double centerFreq, double sampleRate, int gainTenthDb,
                               int fftSize, double fftRate, const std::string& mode,
                               std::string& err) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    if (p) { LOGI("stale shim found on SDRplay start — tearing down"); stopLocked(); }
    auto* impl = new Impl();
    impl->sampleRate = sampleRate;
    impl->fftSize = fftSize;
    impl->fftRate = fftRate;
    impl->rtlCenter.store(centerFreq);
    impl->viewCenter.store(centerFreq);
    impl->audioFreq.store(centerFreq);
    impl->mode = mode.empty() ? "wfm" : mode;
    impl->lastGainTenthDb = gainTenthDb;
    impl->sdrpIndex = index;

    impl->sdrp = std::make_unique<vibe::SdrplaySource>();
    Impl* self = impl;
    impl->sdrp->setSink([self](const int16_t* iq, int n) {
        self->lastIqAt.store(Impl::nowSecs(), std::memory_order_relaxed);
        self->enqueueIqInt16(iq, n, /*blockIfFull=*/false);
    });
    if (!impl->sdrp->open(index, sampleRate, centerFreq, gainTenthDb, err)) {
        delete impl; return -1;
    }

    impl->fftSize = fftSizeForRate(impl->sampleRate);
    impl->startEngine();
    impl->buildAudio();

    int chosen = -1;
    if (int want = g_vsPort.load(); want > 0) {
        try { impl->listener = net::listen(bindHost(), want); chosen = want; }
        catch (...) { impl->listener = nullptr; }
    } else {
        for (int p2 = 48000; p2 < 48050; p2++) {
            try { impl->listener = net::listen(bindHost(), p2); chosen = p2; break; }
            catch (...) { impl->listener = nullptr; }
        }
    }
    if (!impl->listener) {
        err = g_vsPort.load() > 0
            ? "port " + std::to_string(g_vsPort.load()) + " is already in use — choose another"
            : "no free port in 48000-48049";
        impl->teardownAudio(); impl->rx.stop();
        impl->sdrp->close(); delete impl; return -1;
    }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });
    impl->startDspThread();

    p = impl;
    LocalSdrShim::applyDesiredDsp(impl);   // the listener's DSP choices survive this restart
    // ★★★ THE WATCHDOG WAS NEVER STARTED ON THIS PATH. The silence detector, `lastIqAt` and the
    // whole reporting chain already existed and the SDRplay sink even stamped `lastIqAt` on
    // every buffer — but startHotplugWatch() was only ever called from the RTL start, so
    // nothing ever READ it. That is precisely why an RSP stall was silent: the server went on
    // reporting itself healthy because no code was asking whether samples had stopped
    // (Stuart, 2026-07-27). One missing call, in the one place it mattered most.
    impl->startHotplugWatch();
    LOGI("SDRplay started: index=%d center=%.0f rate=%.0f port=%d",
         index, centerFreq, sampleRate, chosen);
    return chosen;
}

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
    if (int want = g_vsPort.load(); want > 0) {
        try { impl->listener = net::listen(bindHost(), want); chosen = want; }
        catch (...) { impl->listener = nullptr; }
    } else {
        for (int p2 = 48000; p2 < 48050; p2++) {
            try { impl->listener = net::listen(bindHost(), p2); chosen = p2; break; }
            catch (...) { impl->listener = nullptr; }
        }
    }
    if (!impl->listener) {
        err = g_vsPort.load() > 0
            ? "port " + std::to_string(g_vsPort.load()) + " is already in use — choose another"
            : "no free port in 48000-48049";
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
    LocalSdrShim::applyDesiredDsp(impl);   // the listener's DSP choices survive this restart
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
    if (int want = g_vsPort.load(); want > 0) {
        try { impl->listener = net::listen(bindHost(), want); chosen = want; }
        catch (...) { impl->listener = nullptr; }
    } else {
        for (int p2 = 48000; p2 < 48050; p2++) {
            try { impl->listener = net::listen(bindHost(), p2); chosen = p2; break; }
            catch (...) { impl->listener = nullptr; }
        }
    }
    if (!impl->listener) {
        err = g_vsPort.load() > 0
            ? "port " + std::to_string(g_vsPort.load()) + " is already in use — choose another"
            : "no free port in 48000-48049";
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
    LocalSdrShim::applyDesiredDsp(impl);   // the listener's DSP choices survive this restart
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
    impl->stopping.store(true);      // so the capture thread knows this is US, not an unplug
    impl->stopHotplugWatch();
    if (impl->dev) rtlsdr_cancel_async(impl->dev);
    if (impl->useSpy()) {
        impl->tcpRunning.store(false);
        impl->spyFftRunning.store(false);
        impl->spyFftCv.notify_all();
        if (impl->spyFftThread.joinable()) impl->spyFftThread.join();
        impl->spy->close();
    }
    if (impl->useTcp()) { impl->tcpRunning.store(false); if (impl->tcpSock) impl->tcpSock->close(); }
    // ★★★ STOP THE CALLBACK SOURCES HERE, BEFORE ANYTHING IS DESTROYED. libairspyhf and the
    // SDRplay API each run their OWN streaming thread and keep calling our sink until told to
    // stop — so leaving it to `delete impl` means that thread reaches a half-destroyed Impl.
    // ★ Observed on the first Airspy build (Stuart, 2026-07-27): refreshing the browser tore
    // the shim down and libairspyhf's consumer thread called enqueueIqFloat() on an Impl whose
    // mutexes had already been destructed — std::mutex::lock() threw system_error and the
    // process SIGABRTed. The stack was unambiguous:
    //     consumer_threadproc -> streamCb -> enqueueIqFloat -> std::mutex::lock -> abort
    // ★ The RSP has exactly the same shape and had only been getting away with it: its Uninit
    // happened in ~SdrplaySource during `delete impl`, i.e. already inside teardown, with
    // member destruction order the only thing standing between it and this same crash. Both
    // are stopped explicitly now, in the same place every other source is.
    if (impl->useAirspyHf()) { impl->ahf->stop(); impl->ahf->close(); }
    if (impl->useSdrplay())  { impl->sdrp->close(); }
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
    // ★★★ NEVER JOIN THE THREAD YOU ARE ON. `joinable()` means "has an associated thread", NOT
    //     "is not me" — so a CONNECTION THREAD that reaches this teardown (a client disconnecting
    //     is exactly that) walked the list and joined ITSELF. std::thread::join() then throws
    //     system_error(EDEADLK), nothing catches it, and the app aborts:
    //         std::__throw_system_error -> __cxa_throw -> std::terminate -> abort
    //     Seen on macOS 10.0.2 build 44 (2026-08-01), with a second caller already inside the
    //     same teardown on the TurboModule queue — two joins racing, one of them fatal.
    //     ★ Detach instead: the thread is finishing anyway (serverRunning is already clear), and a
    //     detached thread that outlives this call is a far smaller problem than a crash.
    //     ★★ AND CATCH. A join can also fail because another caller already joined the same
    //     thread; a teardown path must never be the thing that kills the process — least of all a
    //     teardown running because something ELSE already went wrong.
    const auto self = std::this_thread::get_id();
    auto joinSafely = [&](std::thread& t, const char* what) {
        if (!t.joinable()) return;
        if (t.get_id() == self) {
            LOGI("shim: %s is the calling thread — detaching rather than joining itself", what);
            t.detach();
            return;
        }
        try { t.join(); }
        catch (const std::system_error& e) { LOGI("shim: %s join failed (%s)", what, e.what()); }
    };
    joinSafely(impl->acceptThread, "accept thread");
    { std::lock_guard<std::mutex> lk(impl->connMtx);
      for (auto& t : impl->connThreads) joinSafely(t, "connection thread");
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
    if (!impl->listener) { err = g_vsPort.load() > 0
            ? "port " + std::to_string(g_vsPort.load()) + " is already in use — choose another"
            : "no free port in 48000-48049"; delete impl; return -1; }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });
    p = impl;
    LocalSdrShim::applyDesiredDsp(impl);   // the listener's DSP choices survive this restart
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
    if (p->useSdrplay()) {
        p->lastGainTenthDb = gainTenthDb;
        p->sdrp->setGainTenthDb(gainTenthDb);
        LOGI("gain (RSP): %s", gainTenthDb < 0 ? "auto" : std::to_string(gainTenthDb / 10.0).c_str());
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
// ★ Each of these records the choice in g_dsp FIRST and only then touches the live radio, so
// the value is remembered even when there is no radio to apply it to yet. applyDesiredDsp()
// replays the record onto every newly built Impl.
void LocalSdrShim::setSquelch(bool on, float db) {
    g_dsp.squelchOn.store(on); g_dsp.squelchDb.store(db);
    if (!p) return;
    p->squelchOn.store(on); p->squelchDb.store(db);
    LOGI("squelch: %d @ %.1f dB", on, db);
}
void LocalSdrShim::setNR(bool on) {
    g_dsp.nrOn.store(on);
    if (!p) return;
    p->nrOn.store(on);
    if (!on) { std::lock_guard<std::mutex> lk(p->nrMtx); if (p->nrEng) p->nrEng->reset(); }
    LOGI("audio NR: %d", on);
}
void LocalSdrShim::setNrStrength(float s) {
    g_dsp.nrStrength.store(s);
    if (!p) return;
    std::lock_guard<std::mutex> lk(p->nrMtx);
    if (!p->nrEng) p->nrEng = new AudioNR();
    p->nrEng->setStrength(s);
}
float LocalSdrShim::getNrCpu() { return p ? p->nrCpuPct.load() : 0.0f; }
void LocalSdrShim::setNotch(bool on) {
    g_dsp.notchOn.store(on);
    if (!p) return;
    p->notchOn.store(on);
    if (!on) { std::lock_guard<std::mutex> lk(p->notchMtx); if (p->notchEng) p->notchEng->reset(); }
    LOGI("auto notch: %d", on);
}
void LocalSdrShim::setStereoEnabled(bool on) {
    g_dsp.stereoOn.store(on);
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
bool LocalSdrShim::isAirspyHf() const { return p && p->useAirspyHf(); }

void LocalSdrShim::setSampleRate(double rate) {
    if (!p || rate <= 0) return;
    Impl* impl = p;
    const bool tcp = impl->useTcp();
    const bool rsp = impl->useSdrplay();
    // ★★★ THE AIRSPY WAS MISSING FROM THIS GUARD, so an HF+ — which has no `dev` — returned
    // here and the whole function did nothing. The picker offered the radio's OWN rates, the
    // user chose one, and the server silently discarded it (Stuart, 2026-07-29: "when i chose
    // different options they dont change"). The restart branch for the HF+ was already written
    // at the bottom of this function; it was simply never reachable.
    // ★ FIFTH time this exact shape has bitten: `tcp / rsp / else-means-dongle`. See the note in
    //   the hwinfo rates list, radioCapsJson and resumeCaptureIdle. NAME EVERY SOURCE.
    const bool ahf = impl->useAirspyHf();
    if (!tcp && !rsp && !ahf && !impl->dev) return;
    // Stop the IQ source + drain the DSP consumer BEFORE taking modeMtx (the
    // dspThread locks modeMtx per buffer, so holding it across the join would
    // deadlock). With both quiesced, the rtlsdr control transfer below runs on an
    // idle libusb and the engine rebuild has no concurrent rx.feed.
    if (tcp)      { impl->tcpRunning.store(false); }
    // ★ An RSP keeps streaming across a rate change — the API reconfigures in place, and
    // tearing the device down is what crashed it earlier. Just stop consuming while the
    // engine is rebuilt.
    else if (rsp) { impl->sdrp->setPaused(true); }
    // ★ Same treatment as the RSP: the HF+ reconfigures in place, so pause the consumer rather
    //   than tearing the device down — tearing down is what crashed the RSP earlier.
    else if (ahf) { impl->ahf->setPaused(true); }
    else          { impl->restarting.store(true); rtlsdr_cancel_async(impl->dev); }
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
    } else if (rsp) {
        impl->sdrp->setSampleRate(rate);   // also moves the IF bandwidth to match the span
        actual = (uint32_t)llround(rate);  // the RSP takes the rate it is given
    } else if (ahf) {
        // ★★★ SET THE DEVICE **HERE**, BEFORE THE ENGINE IS BUILT. This used to only compute
        //   the number and leave the actual `ahf->setSampleRate()` to the restart branch at the
        //   BOTTOM of this function — i.e. after startEngine(), buildAudio() AND sendConfig().
        //   So for the whole rebuild the radio was still delivering at the OLD rate into a DSP
        //   chain built for the NEW one, and the client had already been told the new figure.
        //   That is a rate mismatch by construction, and a rate mismatch is heard as a PITCH
        //   SHIFT — chipmunks when the real rate is below what the chain assumes, Barry White
        //   when it is above (Stuart, 2026-08-01: "some sample rates have the chipmunk and
        //   barry white") — and seen as the spectrum lagging the audio while the stale-rate
        //   samples drain through.
        impl->ahf->setSampleRate(rate);
        // ★★★ MEASURE THE RADIO, do not assume — and do not trust its own list either. An HF+
        //   Discovery advertises seven rates, implements THREE (912/456/228), and silently rounds
        //   everything else UP while returning success. `nearestRate()` therefore agreed with the
        //   request and disagreed with reality, so the DSP was built for a rate the radio was not
        //   running: heard as Barry White, slow by exactly the ratio between the two (measured on
        //   hardware 2026-08-01 — see AirspyHfSource::setSampleRate for the full table).
        //   ★ REVERTED to the pre-2026-08-02 form along with the rest of the Airspy path — see the
        //   note in airspyhf_source.cpp. The measured-rate and decimation work is in git and will
        //   come back with verification behind it.
        actual = (uint32_t)impl->ahf->nearestRate(rate);
        // ★★★ RE-APPLY THE TUNED CENTRE. An HF+ runs LOW-IF at some sample rates and zero-IF at
        //   others (airspyhf_is_low_if reports which, and open() logs it), so the offset the
        //   library applies to reach baseband is RATE-DEPENDENT. Changing the rate could
        //   therefore move the signal without anything re-tuning it — "some sample rates the
        //   frequency is off". finishOpen() has always done setSampleRate THEN setFrequency in
        //   that order for exactly this reason; the runtime path did the first and not the
        //   second.
        impl->ahf->setFrequency(impl->rtlCenter.load());
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
    else if (rsp) { impl->sdrp->setPaused(false); }
    else if (impl->useAirspyHf()) {
        // ★ The device rate and the re-tune are applied UP THERE, before the engine is built —
        //   see the long note in the `ahf` branch. Doing it here meant the radio only ever
        //   agreed with the DSP at the rate finishOpen() had set, which is why the HF+ "only
        //   works properly at the maximum sample rate": 912 kHz is the one it opens at, and
        //   every runtime change took this broken ordering.
        impl->ahf->setPaused(false);
    }
    else {
        impl->launchCapture();
        impl->restarting.store(false);   // back to normal: a stop now really is an unplug
    }
    LOGI("sample rate: %.0f (actual %u) fft=%d tcp=%d", rate, actual, impl->fftSize, tcp);
}
void LocalSdrShim::setDeemphasis(double tau) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    g_dsp.deempTau.store(tau);   // ★ remembered even if there is no radio yet
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
    s.deviceLost = p->deviceLost.load();
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
    // ★ An RSP has NO discrete tuner-gain table to read: gain is an LNA state plus a
    // continuous IF gain reduction. The client's slider needs SOMETHING to offer, so present
    // a linear 0-49 dB scale — which is exactly the range setGainTenthDb maps onto IF
    // reduction. ★ Deliberately NOT the dongle's table: showing R820T steps for an RSP would
    // be inventing values the hardware has never heard of.
    if (p->useSdrplay()) {
        std::vector<int> g;
        for (int db = 0; db <= 49; ++db) g.push_back(db * 10);
        return g;
    }
    if (!p->dev) return out;
    int n = rtlsdr_get_tuner_gains(p->dev, nullptr);
    if (n <= 0) return out;
    out.resize(n);
    rtlsdr_get_tuner_gains(p->dev, out.data());
    return out;
}

bool LocalSdrShim::isSdrplay() const { return p && p->useSdrplay(); }

std::string LocalSdrShim::radioCapsJson() const {
    if (!p) return "";
    // ★ AIRSPY FIRST. The RTL branch below is an `if (!useSdrplay())` early return, so ANY
    // third driver falls into it and is reported as a dongle — which is exactly what happened:
    // an Airspy HF+ announced itself as driver "rtl" and the client drew dongle controls for
    // it (Stuart, 2026-07-27). A two-driver test written as "not the other one" silently
    // mis-describes the third.
    if (p->useAirspyHf()) {
        // ★ WHAT THIS RADIO ACTUALLY HAS — the client shows Airspy controls only when it sees
        // this, exactly as it does for an RSP. An HF+ has no LNA STATE table and no IF gain
        // reduction; it has a preamp switch, an 8-step attenuator and its own AGC, so telling
        // the client "sdrplay-shaped" would draw the wrong controls entirely.
        auto& a = *p->ahf;
        std::string j = ",\"radio\":{\"driver\":\"airspyhf\",\"model\":\"" + a.model() + "\"";
        j += ",\"attSteps\":9,\"attStepDb\":6";     // 0..8 => 0..48 dB
        j += ",\"hfLna\":true,\"hfAgc\":true,\"agcThreshold\":true,\"calPpb\":true";
        // ★ THE TUNING HOLE, published. 31-60 MHz does not exist on this hardware, and a client
        // that does not know cannot stop a user parking on a dead frequency and blaming us.
        j += ",\"ranges\":[[500,31000000],[60000000,260000000]]";
        j += ",\"rates\":[";
        const auto& rl = a.sampleRates();
        for (size_t i = 0; i < rl.size(); ++i)
            j += (i ? "," : "") + std::to_string(rl[i]);
        j += "]}";
        return j;
    }
    if (!p->useSdrplay()) {
        // ★ Name the dongle too. The client had nothing to show for a receiver, which is an
        // odd thing for a radio application to be coy about — and the USB descriptor carries
        // what is written on the box ("Blog V4"), which is far more use than "RTL-SDR".
        // The USB descriptor carries what is written on the box ("Blog V4"), which is far
        // more use than librtlsdr's generic tuner name — the same reasoning as vs_device_name.
        std::string n = "RTL-SDR";
        if (p->usbIndex >= 0) {
            char mfr[256] = {0}, prd[256] = {0}, ser[256] = {0};
            if (rtlsdr_get_device_usb_strings((uint32_t)p->usbIndex, mfr, prd, ser) == 0 && prd[0]) {
                n = std::string(mfr[0] ? mfr : "") + (mfr[0] ? " " : "") + prd;
            }
        }
        for (auto& c : n) if (c == '"' || c == '\\') c = ' ';   // device strings, kept simple
        return ",\"radio\":{\"driver\":\"rtl\",\"model\":\"" + n + "\"}";
    }
    auto& d = *p->sdrp;
    std::string j = ",\"radio\":{\"driver\":\"sdrplay\",\"model\":\"" + d.model() + "\"";
    j += ",\"lnaStates\":" + std::to_string(d.lnaStateCount());
    j += ",\"ifGrMin\":20,\"ifGrMax\":59,\"agcSetPoint\":true";
    j += std::string(",\"rfNotch\":") + (d.hasRfNotch() ? "true" : "false");
    j += std::string(",\"dabNotch\":") + (d.hasDabNotch() ? "true" : "false");
    j += std::string(",\"biasT\":") + (d.hasBiasT() ? "true" : "false");
    j += "}";
    return j;
}

// ★★★ EVERY HARDWARE CALL BELOW RUNS UNDER modeMtx. They did not, and that is a strong
// candidate for the audio/RDS freeze that a retune cures:
//
//   • These setters are invoked straight off the WEBSOCKET MESSAGE THREAD (ahf_control /
//     rsp_control), while the DSP thread holds modeMtx per buffer and tuneHw() runs under it.
//   • This file already learned the lesson for TUNING — "modeMtx serialises this against
//     retune()… both call the non-thread-safe tuneHw(); racing them corrupted the tuner PLL
//     (off-tune-until-nudged bug)" — but the gain/notch setters were never brought under it.
//   • libairspyhf and the SDRplay API are not documented thread-safe, and each of these is a
//     USB control transfer into a device that is mid-stream.
//
// ★ Stuart, 2026-07-28, gave the trigger that pointed here: "IF i change any SDR settings in
//   the menu sometimes it will trigger that glitch". Intermittent = a race; cured by a retune =
//   the state the retune rebuilds is what got corrupted; spectrum unaffected = the fault is
//   downstream of the FFT. All three fit.
//
// ★ modeMtx is RECURSIVE and none of these join a thread, so the deadlock trap that applies to
//   stopDspThread() (which joins a thread that itself takes modeMtx) does not apply here.
//
// ★★ UNVERIFIED. The proof is changing settings repeatedly on air and the freeze no longer
//    following. If it still happens, this was not it — do not assume it away.
#define VIBE_HW_LOCK() std::lock_guard<std::recursive_mutex> _hwlk(p->modeMtx)

void LocalSdrShim::setAhfAgc(bool on) {
    g_dsp.ahfAgc.store(on ? 1 : 0);
    if (!p || !p->useAirspyHf()) return;
    VIBE_HW_LOCK(); p->ahf->setAgc(on);
}
void LocalSdrShim::setAhfAgcThreshold(bool high) {
    g_dsp.ahfAgcHigh.store(high ? 1 : 0);
    if (!p || !p->useAirspyHf()) return;
    VIBE_HW_LOCK(); p->ahf->setAgcThreshold(high);
}
void LocalSdrShim::setAhfAttenuation(int steps) {
    g_dsp.ahfAtt.store(steps);
    if (!p || !p->useAirspyHf()) return;
    VIBE_HW_LOCK(); p->ahf->setAttenuation(steps);
}
void LocalSdrShim::setAhfLna(bool on) {
    g_dsp.ahfLna.store(on ? 1 : 0);
    if (!p || !p->useAirspyHf()) return;
    VIBE_HW_LOCK(); p->ahf->setLna(on);
}
void LocalSdrShim::setAhfCalibrationPpb(int ppb) {
    g_dsp.ahfPpb.store(ppb);
    if (!p || !p->useAirspyHf()) return;
    VIBE_HW_LOCK(); p->ahf->setCalibrationPpb(ppb);
}
void LocalSdrShim::setLnaState(int v)       { g_dsp.rspLna.store(v);
                                              if (!p || !p->useSdrplay()) return;
                                              VIBE_HW_LOCK(); p->sdrp->setLnaState(v); }
void LocalSdrShim::setIfGainReduction(int v){ g_dsp.rspIfGr.store(v);
                                              if (!p || !p->useSdrplay()) return;
                                              VIBE_HW_LOCK(); p->sdrp->setIfGainReduction(v); }
void LocalSdrShim::setIfAgc(bool v) {
    g_dsp.rspIfAgc.store(v ? 1 : 0);   // remembered even with no radio yet
    if (!p || !p->useSdrplay()) return;
    VIBE_HW_LOCK();
    // ★★ RECORD THE PREFERENCE; DO NOT CANCEL THE KICK. Cancelling was wrong: the CLIENT
    // re-sends its saved settings the instant it connects, so an "ifagc on" arrived before
    // any samples were flowing — too early for the transition to take, exactly as it was
    // inside open() — and it then suppressed the later kick that would have worked. The AGC
    // was left inert again (Stuart, 2026-07-27: "agc stuck on that load").
    // ★ Turning AGC OFF still cancels it, because the kick only fires when it is WANTED —
    // which is the honest way to respect a manual choice, rather than by racing it.
    p->sdrpAgcWanted = v;
    p->sdrp->setIfAgc(v);
}
void LocalSdrShim::setIfAgcSetPoint(int v)  { g_dsp.rspAgcSet.store(v);
                                              if (!p || !p->useSdrplay()) return;
                                              VIBE_HW_LOCK(); p->sdrp->setIfAgcSetPoint(v); }
void LocalSdrShim::setIfAgcDynamics(int a, int d, int dd, int th) {
    if (!p || !p->useSdrplay()) return;
    VIBE_HW_LOCK(); p->sdrp->setIfAgcDynamics(a, d, dd, th);
}
void LocalSdrShim::setRfNotch(bool v)       { g_dsp.rspRfNotch.store(v ? 1 : 0);
                                              if (!p || !p->useSdrplay()) return;
                                              VIBE_HW_LOCK(); p->sdrp->setRfNotch(v); }
void LocalSdrShim::setDabNotch(bool v)      { g_dsp.rspDabNotch.store(v ? 1 : 0);
                                              if (!p || !p->useSdrplay()) return;
                                              VIBE_HW_LOCK(); p->sdrp->setDabNotch(v); }
void LocalSdrShim::setBiasT(bool v)         { if (!p || !p->useSdrplay()) return;
                                              VIBE_HW_LOCK(); p->sdrp->setBiasT(v); }
#undef VIBE_HW_LOCK

bool LocalSdrShim::isRunning() const { return p != nullptr; }

} // namespace vibe
