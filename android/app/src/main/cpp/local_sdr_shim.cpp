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
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <map>
#include <set>
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
#include "decoders/time_decoder.h"    // MSF / DCF77 time signals
#include "vibe_web_page.h"          // GENERATED: the web client served from GET /
#include "vibe_setup_page.h"
#include "fd_passing.h"
#include <poll.h>
#include <sys/socket.h>   // MSG_PEEK, recv — for the hand-off peek        // hand-written: the setup page, GET / when unconfigured
#include "vibe_admin.h"
#include "vibe_proxy.h"
#include "vibe_admin_ticket.h"
#include "vibe_bands.h"             // the ban list, the connection log and the machine's vitals

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

// ★★★ THE SHARED WATERFALL WINDOW — 1024 bins for every listener who does not ask otherwise.
// Stuart, 2026-08-04, naming the model: *"it needs to function like UberSDR's waterfall — a 1024
// bin window for all users, and zooming resamples so that 1024 bins remains sharp no matter the
// zoom level unless zoomed extremely far out."*
// ★★ 1024 is not a compromise, because RESOLUTION COMES FROM THE ZOOM RESAMPLING, NOT THE WIDTH.
// With --zoom-spectrum the DSP produces real bins at depth, so a 1024-bin window stays sharp all
// the way down; a wider window only buys detail when zoomed far OUT, where there is nothing to
// see anyway. What it costs is linear: measured on the Pi, 4096 bins is 0.50 Mb/s per listener
// and 1024 is about a quarter of that — which on a shared receiver is the difference between
// tens of listeners and hundreds.
// ★ OUT_BINS stays the CEILING (a client may still ask for more, up to a GPU-safe 4096); this is
// only what a client gets when it expresses no preference. The watch still asks for 128.
constexpr int WIRE_BINS_DEFAULT = 1024;

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
// ★★★ Locked hardware centre — see setVibeServerLockedCentre in the header for why. 0 = off.
static std::atomic<double> g_vsLockedCentre{0.0};
// Channel method: false = Direct (per-client DDC), true = Shared (fast convolution). Startup only.
static std::atomic<bool>   g_vsSharedChannels{false};
// Zoom spectrum on/off. It SUPPRESSES the wide path while active, so it needs an off switch.
static std::atomic<bool>   g_vsZoomSpectrum{true};
// ★ Operator-declared RSP front-end filters. OFF by default: the RF notch covers broadcast FM
//   and would destroy an FM receiver's signal, so it is never assumed. See the apply site.
// ★★★ GRACE PERIOD BEFORE THE RADIO IDLE-PARKS, in seconds. NOT a power feature — a HARDWARE
//     PROTECTION one. Parking and waking is the single most dangerous thing we do to a radio:
//     it is what re-enumerates an RSP into a permanent stall, what crashed libusb on the first
//     connect after an idle, and what leaves RDS half-dead on resume. A listener reloading their
//     page, or hopping app->web, used to drive a full park/wake cycle every time.
//     ★ It costs almost nothing to wait: idle-parking does not stop the dongle, it only discards
//       its samples (see pauseCaptureIdle), so an idle receiver already draws what a busy one does.
//     ★ It also guarantees the AGC settle finishes — see the kick, which must not be abandoned
//       half-done by someone who connected and left (Stuart, 2026-08-03).
/** ★★ OFF BY DEFAULT, AND IT MUST STAY THAT WAY. Almost every server is the only thing
 *  using its radio, and for them releasing it buys nothing and costs the spectrogram and
 *  the band-conditions history. This is for the machine that shares one SDR with
 *  OpenWebRX or a decoder, where holding the device idle blocks the other program
 *  entirely (Stuart, 2026-08-07: "just dont enable it by default"). */
std::atomic<bool> g_vsReleaseWhenIdle{false};
static std::atomic<double> g_vsIdleGraceSec{300.0};
static std::mutex          g_vsTuneLimitMtx;
static std::string         g_vsAllowCsv, g_vsBlockCsv;
static vibebands::Ranges   g_vsPermitted;      // empty = no restriction beyond the hardware
static vibebands::Ranges vsPermittedRanges(const vibebands::Ranges& hardware);   // defined below

static std::atomic<bool>   g_vsProvidesSpectrogram{false};
static std::atomic<bool>   g_vsRfNotch{false};
static std::atomic<bool>   g_vsDabNotch{false};
// ★ How many spectrum listeners may attach at once. 1 = the old single-occupant behaviour.
static std::atomic<int>    g_vsMaxUsers{1};

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

/** ★★★ SNAP A LEARNED FM STATION TO ITS CHANNEL. The shared key is 1 kHz — rightly, because
 *  shortwave is a 5 kHz grid and medium wave a 9 kHz one, and a coarse key destroyed real imports
 *  (CB10 at 27.075 and CB11 at 27.085 collapsing into one). But RDS LEARNING IS FM-ONLY, and there
 *  the grid is 100 kHz, so a listener tuned a few kHz off centre filed a SECOND copy of a station
 *  already on the list: 96.100 and 96.110 both FLEX FM, 96.600 / 96.601 / 96.628 all Heart, one of
 *  them wearing a different logo (Stuart, 2026-08-15: "duplicate entries for RDS bookmarks just a
 *  few kHz apart... they need to be at least 50-100 kHz apart to count").
 *  ★★ Snapping fixes the FREQUENCY as well as the duplicate. 96.628 is not where Heart is; it is
 *     where somebody's VFO was. A bookmark exists to be tuned, so it should carry the channel, not
 *     the accident of how it was found.
 *  ★ Only inside the FM broadcast band, and only on this path. Outside it — and for imports, which
 *    keep the 1 kHz key — nothing changes. */
static double bmSnapFm(double hz) {
    if (hz < 87.0e6 || hz > 108.5e6) return hz;          // not FM broadcast: leave it exactly
    return std::round(hz / 100000.0) * 100000.0;         // ITU-R1 is a 100 kHz raster
}

/** Called from the RDS PS callback. */
static void bmLearn(double hzRaw, int pi, const std::string& psRaw) {
    const std::string ps = bmTrim(psRaw);
    const double hz = bmSnapFm(hzRaw);
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

/** ★★★ CLEAR ONE KIND, NOT BOTH. The learned list is a GUESS that decays — a wrong PS decode, a
 *  logo that attached itself to the wrong station, a frequency filed off-channel — and an owner
 *  needs to be able to throw it away without losing the entries they typed or imported by hand,
 *  which are neither guesses nor expiring (Stuart, 2026-08-15: "a clear manually added bookmarks
 *  to server too, but as a SEPARATE button, as usually these will be correct anyway").
 *  ★ The pending (unconfirmed) sightings go with the learned ones: they are the same guess, one
 *    step earlier, and leaving them would let the list refill itself with what was just cleared. */
static void bmClearLearned() {
    std::lock_guard<std::mutex> lk(g_bmMtx);
    for (auto it = g_bookmarks.begin(); it != g_bookmarks.end(); )
        it = it->second.manual ? std::next(it) : g_bookmarks.erase(it);
    g_bmPending.clear();
    bmSaveLocked();
}

/** The typed and imported ones only. Leaves everything RDS has learned in place. */
static void bmClearManual() {
    std::lock_guard<std::mutex> lk(g_bmMtx);
    for (auto it = g_bookmarks.begin(); it != g_bookmarks.end(); )
        it = it->second.manual ? g_bookmarks.erase(it) : std::next(it);
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

// ── ★★★ GAIN LIMITS ─────────────────────────────────────────────────────────────────────────
static std::mutex            g_gainLimMtx;
static vibebands::GainRules  g_gainLimits;
static std::atomic<int>      g_restGain{-1};
static std::atomic<bool>     g_agcLock{false};

/** ★★★ THE COOLDOWN IS WHAT MAKES THE LIMIT REAL. Every client auto-reconnects on close, so a
 *  plain disconnect would be a blip: the same listener would retake the free radio within
 *  seconds and carry on, and the limit would be decorative. After a timeout the address is
 *  refused for this long, which is the window in which somebody else can actually get in.
 *  ★ Keyed on IP, chosen deliberately over the session id: the id is CLIENT-GENERATED, so a
 *  cooldown on it is advisory at best — clear it and you are back in. The cost is a household
 *  behind one router shares a cooldown, which is the accepted trade (Stuart, 2026-07-27). */
static constexpr int        kSessionCooldownSec = 120;

// ── ★★★ THE ADMIN SUBSYSTEM'S STATE — see vibe_admin.h ────────────────────────────────────────
// Bans and the connection log are policy and history, not radio, so they live in their own
// header. These are the single instances the endpoints and the accept path share.
static vibeadmin::BanList  g_vsBans;
/** ★ The owner's notice to listeners ("antenna maintenance in progress"). A FILE, so the front
 *  door and every radio process show the same thing — see vibeadmin::Notice. */
static vibeadmin::Notice   g_vsNotice;
static vibeadmin::ConnLog  g_vsConnLog;
static vibeadmin::History  g_vsHistory;
/** ★★ IDLE RE-LOCK, minutes. 0 = never (and that is NOT the default — see below).
 *  ★★★ WHY THIS EXISTS AT ALL (Stuart, 2026-08-06): "an admin may have opened a session to make
 *  some tweaks and left a decode running and forgotten that they were logged in as admin, which
 *  then means anybody else could then interact with the open tab to meddle." The dangerous state
 *  is not a stolen password — it is an UNATTENDED BROWSER TAB in a shack, a loft or an office,
 *  still holding admin rights hours after the last click.
 *  ★★ IT RE-LOCKS THE CONTROLS, IT DOES NOT END THE SESSION. That distinction is the whole
 *  design: an admin frequently leaves a session running ON PURPOSE (a decode, a recording, a
 *  long listen), so dropping the connection would punish the intended use to defend against the
 *  accidental one. The audio keeps playing and the decoder keeps decoding; only the ability to
 *  CHANGE anything goes away, and the menu's existing password box brings it straight back. */
static std::atomic<int>    g_vsAdminIdleMin{30};
/** False = a local/household receiver: the admin page hides the panels about managing strangers.
 *  Default false so an install from before this setting behaves exactly as it always did. */
static std::atomic<bool>   g_vsPublicSharing{false};
/** The scheduled-update settings, mirrored here purely so the admin page can DISPLAY them. The
 *  daemon owns the actual firing (its 1 Hz loop) — this is a readout, not a second scheduler. */
static std::atomic<int>    g_vsUpdSrvHour{-1}, g_vsUpdSrvDay{-1};
static std::atomic<int>    g_vsUpdAllHour{-1}, g_vsUpdAllDay{-1};
static std::mutex          g_vsMaintMtx;
/** Empty by default: a platform must OPT IN to offering maintenance, so a new one cannot
 *  accidentally inherit buttons that would strand it. */
static std::string         g_vsMaintActions;

// Opus target bitrate (bits/sec) for compressed VibeServer audio — THE link-adaptive lever. 64 kbps
// is a near-transparent FM-stereo default; the client ramps it down over a constrained link.
static std::atomic<int>    g_vsOpusBitrate{64000};

// ── The config API's handlers, registered by the DAEMON (never on a phone) ────────────────────
// See local_sdr_shim.h for why this is a callback and not code in here.
static std::mutex                    g_vsConfigMtx;
static LocalSdrShim::ConfigGetFn     g_vsConfigGet;
static LocalSdrShim::ConfigSetFn     g_vsConfigSet;
static LocalSdrShim::ConfigPersistFn g_vsConfigPersist;
static LocalSdrShim::EibiFn        g_vsEibiFn;
static LocalSdrShim::SolarFn       g_vsSolarFn;
static LocalSdrShim::RadiosFn      g_vsRadiosFn;
static LocalSdrShim::HandoffFn     g_vsHandoffFn;
/// Our own "/r/<serial>" prefix, stripped from every request that arrives with it.
static std::string                 g_vsPathPrefix;
/** ★ The same radio under its other name. Links use the opaque id; older links, bookmarks and
 *  anything already in a browser's history still carry the serial, and both must strip. */
static std::string                 g_vsPathPrefixAlt;
/// ★ What WE last set the bias-T to. The dongle cannot be asked, so we remember.
static std::atomic<bool>           g_biasTeeOn{false};
/** Reboot / restart / update, performed by the daemon. Null on a phone — see adminAction(). */
static LocalSdrShim::AdminActionFn g_vsAdminActionFn;
static LocalSdrShim::AdminLogFn    g_vsAdminLogFn;
static LocalSdrShim::GeoIpFn       g_vsGeoIpFn;
/** ★ Station artwork by PI/ECC/frequency (RadioDNS). Declared with the other handlers rather than
 *  beside its setter, because the ENDPOINT that reads it is thousands of lines above that. */
static LocalSdrShim::StationLogoFn g_vsStationLogoFn;
static LocalSdrShim::LogoCacheClearFn g_vsLogoClearFn;
static LocalSdrShim::AsnFn         g_vsAsnFn;
/** Forward-declared: used on the connection path, defined with the other handler plumbing. */
static std::string vsCountry(const std::string& ip);
static std::string vsAsnLabel(const std::string& ip);
/** ★ The DESIRED-state store (`g_dsp`) lives with the restart-replay plumbing, far below the
 *  hwinfo builder that has to report it. Forward-declared rather than moved: it is read in
 *  exactly one place up here, and hauling the struct to the top would put the replay logic
 *  a long way from the setters it mirrors.
 *  ★★ Tri-state, deliberately — see the sentinel note on DesiredDsp. <0 / nullopt means the
 *     listener never chose, which is NOT the same as "off" and must not be reported as it. */
static int   vsDesiredRfNotch();     // -1 unset, 0 off, 1 on
static int   vsDesiredDabNotch();    // -1 unset, 0 off, 1 on
static float vsDesiredNrStrength();  // <0 = unset
static int   vsDesiredRspBiasT();    // -1 unset, 0 off, 1 on
// The RSP front end as the owner last left it. -1 = never set.
static std::atomic<int> g_vsSavedLna{-1}, g_vsSavedIfGr{-1}, g_vsSavedIfAgc{-1};

/** ★★★ A NEW VALUE EVERY TIME THE PROCESS STARTS. Something that restarts the server needs to know
 *  the server it is now talking to is the NEW one — and "is it answering?" cannot tell you that,
 *  because the OLD process is still answering for the moment between the request and its exit.
 *  The setup page's "Receiver is back up" button appeared on that stale reply and then led to a
 *  dead page (Stuart, 2026-08-06). Comparing an instance id cannot be fooled by timing.
 *  ★ Not a UUID: this only has to differ from the value the caller saw a moment ago. */
static const std::string& vsInstanceId() {
    static const std::string id = [] {
        char b[32];
        snprintf(b, sizeof b, "%llx-%x",
                 (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count(),
                 (unsigned)getpid());
        return std::string(b);
    }();
    return id;
}

/** Merge `patch` into the stored config and write it out. No-op where nothing registered a
 *  handler (the phone and Mac apps). Never throws and never blocks the caller on I/O errors —
 *  losing a saved gain is a nuisance; wedging the control path that set it is not. */
// ★★★ The proxies whose X-Forwarded-For we believe. EMPTY = believe nobody, which is the
//     default and the safe answer: the header is client-supplied text. See vibe_proxy.h.
/** ★★ WHO IS LOOKING AT THE LANDING PAGE. A visitor choosing a radio is not connected to one, so
 *  they appear nowhere in any listener list — the owner sees "nobody here" while somebody is
 *  reading the page (Stuart, 2026-08-08: "It should also show if they are sitting on the landing
 *  page or not"). The page refreshes its spectrogram every 15s while it is open, which makes a
 *  perfectly good heartbeat: last-seen per address, and anything older than a minute has gone.
 *  ★ Addresses only, held in memory, pruned — the same data the connection log already keeps. */
static std::mutex g_vsVisitorMtx;
static std::map<std::string, double> g_vsVisitors;
static void vsNoteVisitor(const std::string& ip) {
    if (ip.empty()) return;
    const double now = (double)::time(nullptr);
    std::lock_guard<std::mutex> lk(g_vsVisitorMtx);
    g_vsVisitors[ip] = now;
    for (auto it = g_vsVisitors.begin(); it != g_vsVisitors.end();)
        it = (now - it->second > 60.0) ? g_vsVisitors.erase(it) : std::next(it);
}

static LocalSdrShim::RtlSerialFn g_vsRtlSerialFn;
static LocalSdrShim::RtlSerialStatusFn g_vsRtlSerialStatusFn;

static vibeproxy::TrustedProxies g_vsTrustedProxies;
/** ★ Whether the owner has named ANY proxy, and how many listeners have arrived from loopback.
 *  Together they answer "is this server behind something it has not been told about?" */
static std::atomic<bool> g_vsHaveTrustedProxies{false};
static std::atomic<int>  g_vsLoopbackSessions{0};
static std::mutex                g_vsTrustedProxiesMtx;

static void vsPersist(const std::string& patch) {
    LocalSdrShim::ConfigPersistFn fn;
    { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsConfigPersist; }
    if (fn) fn(patch);
}
// ★ Has the owner finished browser setup? NOT the same question as "is an admin password set".
static std::atomic<bool>             g_vsConfigured{false};
/** True when the host has its own settings UI (macOS, Android) — then the browser setup wizard is
 *  never served, because it would be a second way to configure one server. */
static std::atomic<bool>             g_vsNativeSetup{false};

// ── ★★★ WHERE A NEW SESSION STARTS — the owner's landing frequency and mode ──────────────────
// The VFO is SERVER state and survives the listener who moved it, which is correct for a shared
// receiver mid-session and wrong for the first person through the door: Stuart set 7074 USB,
// connected, and landed wherever the previous session had left the radio (2026-08-05).
// ★★ APPLIED WHEN THE LISTENER COUNT GOES 0 -> 1, not on every connect. Applying it per-connect
//    would YANK a group already listening together every time somebody joined — the shared
//    receiver's whole premise is one radio, one VFO, one view. A fresh session is the only moment
//    at which "where new listeners start" is unambiguous.
static std::atomic<double>  g_vsLandingHz{0.0};
static std::mutex           g_vsLandingMtx;
static std::string          g_vsLandingMode;

// ★★★ REMOVED 2026-08-04: g_vsOutBins, the client-requestable waterfall width as a GLOBAL.
// Its own comment justified it — "Single-occupant, so one global suffices" — and `--users 20`
// repealed that without anyone revisiting the line. The most recent client to connect set the
// width for EVERYBODY: a watch asking for 128 cut every browser on the server to 128 bins, and
// they never recovered. Width is now PER CLIENT (Impl::clientBins); read it with binsFor(sock),
// and use wireBins() for the one thing that must be single-valued — the DSP-side zoom FFT, which
// runs at the widest listener's width so narrower ones can be peak-held down from it.
// ★ If you are about to add another global for something a client chooses, this is the third time.

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

/** ★ Adapter so vibe_admin_ticket.h can stay free of this file's internals — it takes the MAC as a
 *  function so it is testable on its own, and this is the real one. */
static void vsTicketMac(const void* key, size_t klen, const void* msg, size_t mlen, uint8_t out[32]) {
    hmacSha256((const uint8_t*)key, klen, (const uint8_t*)msg, mlen, out);
}
static int64_t vsNowEpoch() { return (int64_t)::time(nullptr); }

/** Mint an admin ticket that EVERY radio on this machine will accept. See vibe_admin_ticket.h:
 *  the per-process nonce cannot cross a process boundary, and with a radio per process that is
 *  every interesting case. */
static std::string vsMintAdminTicket() {
    std::string secret;
    { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
    return vibeadmin::mintTicket(secret, vsNowEpoch() + vibeadmin::kTicketTtlSec, &vsTicketMac);
}
/** ★★★ ONE PLACE THAT KNOWS WHAT ADMIN PROOF LOOKS LIKE. There were FIVE copies of
 *  "nonce + token + not locked out + verify", and adminOkFor's own comment warns why that is
 *  dangerous: "a second endpoint cannot re-implement it slightly differently — the failure lockout
 *  and the empty-secret rule are both easy to leave out, and either omission is a hole." Adding
 *  ticket support to five copies would have been five chances to miss one, and a missed one is an
 *  endpoint that refuses a legitimate admin (or worse, one that accepts anybody).
 *  ★ `present` is kept separate from `ok` because a request carrying NO credentials must not count
 *    as a failed guess — otherwise any scanner locks the owner out of their own server. */
std::string queryParam(const std::string& reqLine, const char* key);   // defined below
/** `present` = credentials were offered at all. `guessable` = they were of a kind an attacker
 *  could be TRYING — i.e. a password-derived token. A TICKET is neither guessed nor typed: it is
 *  an HMAC over the admin secret, so a bad one means expired or stale, never "someone is having a
 *  go". Counting those as failures locked the OWNER out of their own server: an admin page whose
 *  ticket had lapsed re-polled every two seconds, each poll scored as a wrong password, and the
 *  backoff then refused the correct password typed into the menu (Stuart, 2026-08-08: "refusing
 *  my admin password in this menu now ... so I cannot get my admin session back").
 *  ★ Same principle the no-credential rule already encodes: only a real guess counts as a guess. */
struct VsAdminProof { bool present = false; bool ok = false; bool guessable = false; };
static bool vsTicketOk(const std::string& ticket);
static VsAdminProof vsAdminProof(const std::string& secret, const std::string& reqLine) {
    VsAdminProof p;
    const std::string nonce  = queryParam(reqLine, "vs_admin_nonce");
    const std::string token  = queryParam(reqLine, "vs_admin_auth");
    const std::string ticket = queryParam(reqLine, "vs_admin_ticket");
    p.present   = (!nonce.empty() && !token.empty()) || !ticket.empty();
    p.guessable = (!nonce.empty() && !token.empty());
    if (secret.empty() || !p.present) return p;
    // A ticket proves the same thing as the handshake and works across processes — see
    // vibe_admin_ticket.h. Checked first because it is the cheap, stateless one.
    if (!ticket.empty() && vsTicketOk(ticket)) { p.ok = true; return p; }
    if (!nonce.empty() && !token.empty()) p.ok = g_vsAuthState.verify(secret, nonce, token);
    return p;
}

static bool vsTicketOk(const std::string& ticket) {
    std::string secret;
    { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
    return vibeadmin::verifyTicket(secret, ticket, vsNowEpoch(), &vsTicketMac);
}

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
    int  ahfIndex  = 0;
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
    /** ★★ THE RF GAIN THE KICK STARTS FROM, expressed the way the UI expresses it: a slider
     *  POSITION out of (lnaStateCount-1), where higher = more RF gain. 7/9 on an RSP1B is the
     *  working point Stuart runs the demo at, and it maps to LNA STATE 2 (state counts the other
     *  way — 0 is maximum RF gain). Derived rather than hard-coded to 2 so a model with a
     *  different ladder (an RSP1 has 4 states, a dx has 28) lands somewhere sane instead of
     *  wherever the literal happened to point. */
    static constexpr int kRspInitRfGainPos = 7;
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
    /** Scratch for the band-edge floor percentile. A member so the per-frame measurement does not
     *  allocate; only ever touched on the DSP thread. */
    std::vector<float> edgeBins_;
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
    // ★ Weak-signal processing (high-blend + audio high-cut). Defaults ON: it only ever acts on a
    //   signal that needs it, so a listener who never touches it should get the better sound.
    std::atomic<bool>  weakProcOn{true};
    std::atomic<bool>  imsOn{true};        // adaptive IF — a neighbour, not noise
    std::atomic<bool>  ceqOn{true};        // blind channel equaliser — a reflection
    std::atomic<bool>  nbOn{true};         // noise blanker — impulses
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
    /** ★★★ THE DEVICE HANDLE, AND EVERY CONTROL CALL THAT TOUCHES IT.
     *
     *  Closing a device while another thread is tuning it is a use-after-free, and on an
     *  unattended server libusb ABORTS rather than returning an error. That is exactly why
     *  reopenDevice() sat here for months marked "NOT CALLED — kept as the skeleton of the real
     *  fix": nothing serialised rtlsdr_set_gain / tuneHw / setFftRate against a close.
     *
     *  ★★ LOCK ORDER: devMtx BEFORE modeMtx, never the reverse. setSampleRate takes both.
     *  ★★ THE CAPTURE THREAD NEVER TAKES THIS. It blocks inside rtlsdr_read_async for as long as
     *     the stream runs, so taking devMtx there would mean holding it forever — and releasing
     *     the radio joins that thread while holding devMtx. The stream is always STOPPED first;
     *     serialising it as well would deadlock the very path this exists for.
     *  ★ Recursive because the release path calls control helpers that lock it themselves.
     */
    std::recursive_mutex devMtx;
    /** True while the radio is deliberately let go for another program (see releaseRadio).
     *  Control calls become no-ops rather than touching a handle that is not there. */
    std::atomic<bool> radioReleased{false};

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

    // ★★★ AND IT IS A DONGLE WORKAROUND, SO ONLY THE DONGLE GETS IT. tuneHw() applied the offset
    //     to EVERY driver, and the compensations below subtracted it again — which cancels only if
    //     the radio actually landed 15 kHz high. The HF+ does not: libairspyhf adds its own 5 kHz
    //     IF shift and rotates the remainder away, so it delivers baseband centred exactly where
    //     it was asked. The result was a constant 15.00 kHz error, in AUDIO as well as spectrum,
    //     measured across the whole of medium wave with 12 of 12 carriers agreeing to within 70 Hz
    //     (Stuart, 2026-08-09 — Radio Caroline on 648 reading 663).
    // ★★★ THE NUMBER GAVE IT AWAY IN THE END: the error was not "about 15 kHz", it was 15000 Hz,
    //     and HW_OFFSET_HZ is 15000.0. Three theories died before anyone noticed that — the crop,
    //     the sample rate, and a cross-radio settings leak. Stuart's "we had this working in simple
    //     mode on Android and Mac" is what finally pointed at the server rather than the radio.
    // ★★ An RSP is zero-IF too and has its own DC handling; a network source has no DC spike of
    //    ours to dodge. Only the R820T needs this.
    double hwOffsetHz() const {
        return (useSdrplay() || useAirspyHf() || useTcp() || useSpy()) ? 0.0 : HW_OFFSET_HZ;
    }

    // Physical DC of the FFT = rtlCenter + HW_OFFSET_HZ, so the VFO (at audioFreq)
    // sits HW_OFFSET_HZ below DC.
    double vfoOffsetNow() { return audioFreq.load() - rtlCenter.load() - hwOffsetHz() + demodOffset; }

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

        // ★★★ A LOCKED CENTRE NEVER MOVES — AND THIS IS THE PATH THAT WAS STILL MOVING IT.
        //     retune() honours the lock, but a PAN or ZOOM does not go through retune(): it comes
        //     here and retunes the hardware itself. So with the client's VFO CENTRE set to LOCKED
        //     — where every tune sends a zoom — tuning still dragged the radio, and rtlCenter with
        //     it. Every frequency in the zoom spectrum is measured FROM rtlCenter, which is why
        //     the display drifted while the audio (which re-derives its offset each time) stayed
        //     right (Stuart, 2026-08-02). Half a lock is not a lock: guard every path that tunes.
        const double lockC = g_vsLockedCentre.load();
        if (lockC > 0.0) return lockC;

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
        uint32_t hz = (uint32_t)llround(logicalCenter + hwOffsetHz());
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
                // Copy the socket out, THEN send. tuneHw() runs under modeMtx, and holding
                // clientMtx across a send used to invert the lock order the spectrum path uses.
                // ★ Sends no longer block or take a shared lock (see the Outbox block), so this is
                //   now belt-and-braces rather than load-bearing — kept because copy-then-send is
                //   the right shape regardless.
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

    // ── RDS, PER LISTENER ────────────────────────────────────────────────────────────────────
    // ★★★ ONE SET OF THESE PER LISTENER, because RDS identifies ONE carrier and every listener is
    //     on their own. The whole block used to live on Impl — a single station's worth of state
    //     for the whole server — which was invisible while there was one VFO and wrong the moment
    //     there were thirty: "what if user 1 is listening to Heart and user 2 is listening to
    //     Radio 1? The RDS needs to be independent" (Stuart, 2026-08-05).
    //     ★ The first fix for this designated an OWNER — one WFM listener drove the shared state.
    //       That is strictly better than nothing and still wrong: it shows user 2 user 1's
    //       station. Naming a limitation does not make it acceptable.
    //     ★★ `lastSent*` live here too, so change-detection is per listener as well. Sharing THAT
    //        would mean the first peer to be told suppressed the message for everyone else — a
    //        broadcast built on single-listener bookkeeping.
    struct RdsState {
        std::mutex rdsMtx;
        std::string rdsPsName, rdsText;
        int rdsPi = -1;
        int rdsEcc = 0;                          // RDS Extended Country Code (0 = none)
        int rdsBer = -1;                         // RDS block error rate %, -1 = unknown
        float rdsSig = -99.0f;                   // 57 kHz level vs pilot, dB (-99 = none)
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
        // ★ Every AF glimpsed with a confirmed flag — the list, not just the score. See afAll.
        std::vector<int> rdsAfAll; std::vector<unsigned char> rdsAfAllOk;
        std::vector<float> rdsConst;
        std::vector<float> rdsMpx;             // MPX spectrum, dB per bin
        std::atomic<bool> stereoDetected{false};
        // Last values pushed to THIS listener (change-detect, to avoid marquee re-trigger).
        float lastSentSig_ = -999.0f;
        int lastSentBer_ = -2;   // -2 = never sent (distinct from -1 = decoder has no window)
        std::string lastSentPs_, lastSentRt_;
        int lastSentPi_ = -2; int lastSentEcc_ = -1; bool lastSentStereo_ = false;
    };

    /** This server's own RDS — used only when there is NO per-client pipeline (the phone and Mac
     *  apps, where there is one listener and it is the user). */
    RdsState rdsS;
    /** ★★★ ONE RDS DECODER PER STATION, NOT PER LISTENER. RDS describes the CARRIER, so everyone
     *  tuned to 96.6 would decode byte-identical data — and decoding it thirty times over costs
     *  roughly triple the FM DSP budget for nothing (measured: 20 WFM listeners at 61-102% of real
     *  time each-decoding, 35-38% sharing). One listener per frequency runs it; the rest read the
     *  result, which is not an approximation — it is the same broadcast.
     *  ★★ This is what makes per-listener RDS AFFORDABLE. Independence was never the expensive
     *     part; DUPLICATION was, and the two got conflated because they arrived together.
     *  ★ Keyed to 1 kHz. FM stations sit 100 kHz apart, so this is tolerant of a listener being a
     *    few hundred Hz off and still cannot merge two different stations.
     *  Guarded by clientMtx. */
    struct ClientDsp;                       // ★ defined further down; only the pointer is needed here
    std::map<long long, ClientDsp*> rdsFreqOwner;
    static long long rdsKeyOf(double hz) { return (long long)llround(hz / 1000.0); }
    std::atomic<bool> rdsxOn{false};         // a client has the Advanced RDS decoder open
    double rdsxLastAt = 0.0;                 // wall clock of the last rdsx — see the emit site
    // ★★ ADMIN UNLOCK, per connected client. Cleared whenever the spectrum client changes, so
    // an unlock cannot outlive the session that earned it and be inherited by the next visitor.
    std::atomic<bool> adminOk{false};
    /** Monotonic seconds at the last CONTROL message from the client, for the idle re-lock.
     *  0 = never. See enforceAdminIdle() for why this is stamped on commands and not on
     *  traffic — a streaming session with nobody in the room sends frames forever. */
    std::atomic<double> lastAdminTouch{0};
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
    /** ★ What is attached, for the admin view — "RTTY" reads better than a null pointer check,
     *  and it is the one thing that says WHY a listener is costing more than the others. */
    std::string currentDecoder;
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
    /** MSF / DCF77 time signals. Audio-path like the rest — see decoders/time_decoder.h. */
    TimeDecoder* timeDec = nullptr;
    std::string  morseWord_;                ///< RWM's callsign, accumulated for display
    int  sstvDecim = 0; float sstvAcc = 0.0f;
    std::mutex dxSendMtx;
    std::shared_ptr<net::Socket> dxClient;
    /** ★★ WHOSE AUDIO THE DECODERS ARE LISTENING TO. Empty = the shared pipeline (a personal
     *  receiver, where there is only one). On a shared receiver the decoders must follow the
     *  listener who OPENED them — fed from the shared VFO they decode a signal nobody chose,
     *  which is what they did before per-client tuning existed. */
    std::string decoderSession;
    /** Is any decoder socket open? Read on every listener's audio path, so it must be cheap. */
    std::atomic<bool> decoderAttached{false};
    /** ★★ HOW MANY SAMPLES THE DECODERS HAVE ACTUALLY BEEN GIVEN. Exposed on the admin page.
     *  ★★★ This exists because "the decoder is attached" and "the decoder is being fed" looked
     *  identical from outside for a whole evening — WEFAX missed an entire transmission and the
     *  UI said `decoding…` throughout. A counter that only moves when audio really reaches the
     *  decoder is the difference between "no signal" and "no samples", and those need opposite
     *  fixes: one is an antenna, the other is us. */
    std::atomic<uint64_t> decoderFedSamples{0};
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
    /** ★★★ ADDITIONAL SPECTRUM LISTENERS. `specClient` stays the FIRST one so every existing path
     *  keeps working untouched; these are the rest. They receive the same frames, config and state
     *  — one radio, one VFO, one view — which is the shared-receiver model, and the channelizer is
     *  what makes serving them nearly free (+0.02% of a core each, measured).
     *  ★ Guarded by clientMtx, like specClient. Copy the list under the lock and send OUTSIDE it —
     *    still the rule, though the reason has changed: sends are now queued, not blocking, so this
     *    is about not holding clientMtx across work rather than about a lock-order inversion. */
    std::vector<std::shared_ptr<net::Socket>> specExtra;
    /** ★★★ EACH LISTENER'S OWN WATERFALL WIDTH. Guarded by clientMtx, alongside the sockets.
     *
     *  THE BUG THIS REPLACES (2026-08-04). The requested bin count lived in ONE GLOBAL
     *  (`g_vsOutBins`), set by whichever client connected most recently — and its declaration
     *  said why that was thought safe: *"Single-occupant, so one global suffices."* `--users 20`
     *  made that false and nothing revisited it. Measured on the live Pi: a browser streaming at
     *  4096 bins was silently CUT TO 128 the moment a watch connected, and never recovered — a
     *  32x loss of resolution on someone else's screen, caused by a stranger joining. It works the
     *  other way too: a desktop arriving quadruples the bytes pushed at a phone that asked for 1024.
     *  ★★ Same family as the other per-client-state-in-globals bugs. The tell is always the same:
     *  a comment justifying a global by an occupancy assumption a later feature quietly repealed. */
    std::map<net::Socket*, int> clientBins;

    /** ★★★ THE FRAME RATE EACH LISTENER ASKED FOR. Absent/0 = the server's own default.
     *
     *  THE BUG THIS REPLACES (2026-08-09), and it is the map above's twin. `type:"fftRate"` went
     *  straight to LocalSdrShim::setFftRate() — the ENGINE rate, one number for the whole radio.
     *  So one listener's idle-saver dropping to 5 fps dropped the waterfall to 5 fps for EVERY
     *  listener on that radio, and because nothing recomputed it, it STAYED there after that
     *  listener left. Measured on the public demo's shared RSP1B: the per-second frame counts
     *  stepped between 20 and 5 in blocks of many seconds with the observer touching nothing —
     *
     *      21 20 20 21 20 20 21 20 20 21 20 21 20 20 21 20 20 21 20 20 21 20 20 8 5 6 5 5 5 5 5
     *
     *  — which is what "runs smooth, then sticks, then smooth" is, from the other end. Confirmed
     *  against a local shared-mode server: a bystander who asked for nothing went 24.5 -> 4.9 fps
     *  when a second client asked for 5, and did NOT recover when it left
     *  (`tools/vibeserver-probes/fpsleak.mjs`).
     *
     *  ★★ IT LOOKS LIKE NETWORK JITTER AND IS NOT. The frames that were sent still arrived
     *     promptly (delay-above-best p95 ~11 ms through a Cloudflare tunnel) and no stall was
     *     followed by a catch-up burst — there were simply FEWER FRAMES. That distinction decides
     *     the fix: a client-side pacing buffer absorbs clumping and can do nothing whatever about
     *     frames that were never sent. See tools/vibeserver-probes/jitterprobe.mjs, which prints
     *     the per-second timeline precisely so the two cannot be confused again.
     *
     *  ★ THE ENGINE RUNS AT THE MAXIMUM ANYONE WANTS; everyone slower is decimated on the way
     *    out. So the idle phone costs itself frames and nobody else — one user at 5 fps must not
     *    drag down the other nine. The FFT is shared and already paid for, so serving the fast
     *    listeners costs no more than it did before. */
    std::map<net::Socket*, double> clientFps;
    /** Phase accumulator per listener, advanced once per emitted frame. Fractional, so a client
     *  asking 7 fps off a 20 fps engine gets 7 — not the nearest integer divisor. */
    std::map<net::Socket*, double> clientFpsAcc;
    /** The rate the OWNER configured (--fps / the GUI). The floor when nobody has asked, and what
     *  the radio returns to when the last slow listener leaves. */
    double baseFftRate = 0.0;

    // ── ★★★ PER-CLIENT DSP — every listener their own VFO, demodulator and audio ─────────────
    //
    // THE MODEL, in Stuart's words (2026-08-05): *"it needs to act like an OWRX profile whereby
    // everybody gets their own vfo and demodulator/decoders within that frequency range."*
    // Until now the shim was the opposite shape by design — "one radio, one pipeline, one server
    // per process", ONE `audioFreq`, ONE `mode` — so tuning in one browser window retuned every
    // other listener.
    //
    // ★★ HOW IT IS AFFORDABLE. Not one full-rate pipeline per listener: that runs an NCO over the
    // whole capture for each of them and was measured at +13% of a core each, out of road at
    // about eight. Instead one shared forward FFT (Channelizer, ka9q-style fast convolution), a
    // narrow slice extracted per listener, and a pipeline running at the CHANNEL rate — measured
    // in tools/spike-per-client-channel.cpp at **+0.09% of a core per listener**, with 30
    // listeners costing 40% more than none.
    //
    // ★★★ ONLY IN SHARED/LOCKED MODE. A personal receiver has exactly one listener who owns the
    // radio, and the phone and Mac builds compile this same file — so when the centre is not
    // locked, none of this is constructed and the original single-pipeline path runs untouched.
    // The riskiest thing here would be changing behaviour for the shipping app.
    struct ClientDsp {
        std::shared_ptr<net::Socket> spec;      // whose channel this is
        std::shared_ptr<net::Socket> audio;     // its own audio socket, or null until one opens
        std::string session;
        /** ★ What connected — app, watch, browser or bot. Already recorded in the connection LOG;
         *  kept here too so the LIVE listener list can show it, which is where an owner looks when
         *  deciding whether the address they are about to block is a person (Stuart, 2026-08-08:
         *  "in the ip can we show the user Agent please"). */
        std::string agent;
        std::atomic<unsigned long long> dspNanos{0};   // total time spent in THIS listener's DSP
        // Previous sample, so a rate can be shown without the reader keeping state per listener.
        unsigned long long lastDspNanos = 0, lastSentBytes = 0;
        double             lastSampleAt = 0;
        double vfoHz = 0;                       // ABSOLUTE, like audioFreq
        /** ★★★ THE CAPTURE CENTRE THIS LISTENER'S CHANNEL WAS CUT AGAINST. The extract offset is
         *  `vfoHz - rtlCenter`, computed once in clientRetune — so when the hardware MOVES, every
         *  per-listener channel is silently cutting the wrong part of the spectrum until something
         *  happens to re-tune it. The deferred dongle move makes that routine: the request is
         *  remembered and applied a moment later by the DSP loop, long after clientRetune ran.
         *  ★★ A PULL, not a push. The alternative — having the mover walk `clientDsp` — means
         *     taking clientMtx from the DSP thread while it holds modeMtx, which is the lock order
         *     that deadlocked this server once already. Comparing a double costs nothing and
         *     cannot deadlock. */
        double tunedAtCentre = 0;
        std::string mode = "am";
        double bwHz = 0;
        int    chanBins = 0;                    // power of two dividing fftSize
        double chanRate = 0;
        std::unique_ptr<vibedsp::RxPipeline> rx;
        std::vector<cf32> slice;
        // Audio encode is per client too: two listeners may want different codecs, and an Opus
        // encoder carries stream state that cannot be shared.
        bool wantsOpus = false, forceMono = false;
        // ★★ THIS LISTENER'S OWN VIEW. Per-user zoom falls out of per-user tuning: the narrow
        //    stream already exists, so a zoom FFT on it is nearly free. `viewSpanHz` 0 = zoomed
        //    out, i.e. served by the SHARED wide waterfall like everyone else.
        double viewCentreHz = 0, viewSpanHz = 0;
        bool   ownView = false;      // true while this listener's own zoom is feeding its display
        // ★★★ THE VIEW GETS ITS OWN CHANNEL, SEPARATE FROM THE AUDIO ONE.
        //     Sizing one channel for both meant the private view only existed once the span fell
        //     inside the DEMODULATOR's channel — about 25 kHz for AM. Between there and the point
        //     the shared FFT runs out (~250 kHz at 8 MSPS over 1024 bins) the waterfall was the
        //     wide row stretched 5x: blocky, and blocky exactly across the most useful zooms
        //     (Stuart, 2026-08-05: "it goes really blocky before having a little hitch when
        //     zooming in further and it sharpens up... I suspect it isnt in full KA9Q mode until
        //     max zoom").
        //     ★★ Two channels, not one, because they have opposite requirements: the audio channel
        //     must NEVER be resized (a rebuild restarts the AGC and ducks the audio — that was
        //     this morning's bug), and the view channel must be resized on every zoom. Trying to
        //     serve both from one is what forced the compromise.
        std::unique_ptr<vibedsp::RxPipeline> viewRx;
        std::vector<cf32> viewSlice;
        int    viewChanBins = 0;
        double viewChanRate = 0;
        /** ★★★ TRUE FROM A ZOOM UNTIL THE PRIVATE VIEW ACTUALLY PRODUCES A FRAME.
         *  A zoom that changes decimation empties the zoom FFT's accumulator, and it cannot emit
         *  anything until it has refilled — fftN_ samples of a now-much-narrower stream, which at
         *  a 3 kHz span is about a SECOND. Deeper zoom, longer wait, which is why the stall only
         *  showed at the deepest levels (Stuart, 2026-08-05: "a hitch moving into the most zoomed
         *  in levels where the spectrum stops for a split second").
         *  ★★ The old samples are at the wrong rate so they cannot be kept — but nothing requires
         *  us to go QUIET while it fills. Keep sending the shared wide row, cropped to the new
         *  view, until the sharp one is ready: the picture keeps moving and simply becomes
         *  sharper a moment later, instead of freezing and then snapping. */
        std::atomic<bool> viewPriming{false};

        // ── This listener's own audio effects ────────────────────────────────────────────────
        // ★★★ PER LISTENER, because the audio is. NR, the auto notch, de-emphasis and stereo all
        //     used to be set on the SHARED pipeline — which in per-client mode feeds nobody, so
        //     every one of them moved a control and changed nothing audible (Stuart, 2026-08-05:
        //     "the NR and I assume auto notch is not working, the controls move but no audible
        //     difference, I assume FM de-emphasis and WFM stereo will be the same issue too" —
        //     and he was right about all four).
        //     ★ FOURTH instance of the same family in one session: when a shared resource becomes
        //       per-listener, EVERY path that identified the listener must be re-derived.
        //     ★★ They are also settings a listener may choose FOR THEMSELVES: one person's noise
        //        reduction has no business being applied to everyone else's audio.
        /** ★ THIS listener's RDS — see RdsState. Independent of every other listener's, which is
         *  the whole point: two people on two stations get two station names. */
        RdsState rdsS;
        /** The frequency this listener's RDS belongs to. RDS is about ONE carrier, so the moment
         *  the dial moves it is stale — this is what notices. */
        double rdsAtHz = -1;
        /** Is THIS listener the one decoding RDS for its frequency? See rdsClaim(). */
        bool rdsDecode = false;
        std::atomic<bool> nrOn{false}, notchOn{false};
        float             nrStrength = 0.5f;
        double            deempTau = -1.0;      // <0 = never set; leave the pipeline's own default
        std::atomic<bool> stereoOn{true};
        // ★★★ THE FOUR BROADCAST-FM TREATMENTS, PER LISTENER. Held HERE and not only on the
        //     pipeline, because a mode or width change BUILDS A NEW RxPipeline with fresh
        //     defaults — the exact trap the re-apply block below already warns about for
        //     de-emphasis and stereo: "the control still ON in their UI and no longer doing
        //     anything, which is the WORST version of this bug because it looks like it is
        //     working". Seeded from the server's configured values when the listener arrives, so
        //     the owner still sets the house default and only this listener can change it.
        std::atomic<bool> wspOn{true}, imsOn{true}, ceqOn{true}, nbOn{true};
        // ★★★ ADMIN IS PER LISTENER TOO, and the radio-wide flag it replaces was already
        //     DESCRIBED as "per connected client" while being one atomic for the whole process.
        //     On a shared receiver that means two owners share one bit: whoever unlocks last
        //     speaks for both, and — worse — a stranger arriving is a "new occupant", which
        //     CLEARS it, so an admin listening quietly was demoted by somebody else walking in.
        // ★ Seeded from the credential proved at the handshake, so an owner who arrives holding a
        //   ticket lands unlocked without typing anything, exactly as before.
        std::atomic<bool> adminOk{false};
        std::atomic<double> lastAdminTouch{0};
        std::mutex        fxMtx;
        AudioNR*          nrEng = nullptr;
        AutoNotch*        notchEng = nullptr;
#ifdef VIBE_HAVE_OPUS
        vibe::OpusAudioEncoder opus;
#endif
        Impl* owner = nullptr;
        std::mutex mtx;                         // guards retune against this listener's own thread
        // ── ★★★ ONE DSP THREAD PER LISTENER, which is how OpenWebRX does it ────────────────
        // The DSP thread no longer waits for anybody: it copies the block once, hands every
        // listener a reference and moves on. A listener that falls behind drops ITS OWN blocks
        // and nobody else notices — with a barrier, the slowest listener set the pace for the
        // radio itself.
        // ★ It also lets the kernel schedule listeners across cores as it sees fit, which is the
        //   point Stuart made: "give each user their own DSP thread that the CPU can assign".
        std::thread                                   th;
        std::mutex                                    qm;
        std::condition_variable                       qcv;
        // ★ The block AND which block it was — the phase correction is meaningless without it.
        struct Block { std::vector<cf32> bins; long long index; };
        std::deque<std::shared_ptr<const Block>> q;
        std::atomic<bool>                             run{true};
        std::atomic<uint64_t>                         dropped{0};
        /** ★ This listener's own extract scratch — see Channelizer::ExtractCtx. Sharing one made
         *  every listener's channel land somewhere other than it asked for. */
        vibedsp::Channelizer::ExtractCtx ectx, vectx;
    };
    std::map<net::Socket*, std::shared_ptr<ClientDsp>> clientDsp;
    /** The session we have already landed on the owner's chosen frequency. Guards against landing
     *  the same listener again every time one of their sockets arrives to an empty room. */
    std::string landedSession;
    /** Audio sockets that arrived before their spectrum socket, by session id. */
    std::map<std::string, std::shared_ptr<net::Socket>> pendingAudio;
    /** ★★★ A SESSION THAT HAS ALREADY TUNED IS NOT A FRESH ARRIVAL. The app's AUDIO socket opens
     *  first and carries the tune, so a listener restoring 90.1 MHz had it applied — and then the
     *  SPECTRUM socket arrived a moment later, the landing gate called it a new session and put
     *  the radio back on the owner's landing frequency. The listener saw 90.1 on the dial, a blank
     *  spectrum, and heard 648 kHz; one touch of the dial "fixed" it (Stuart, 2026-08-16).
     *  ★★ The landing exists so a NEW visitor gets the owner's choice rather than wherever the
     *     last person left the radio. Someone who has just said where they want to be is not that
     *     person, and overruling them is the opposite of what the feature is for.
     *  ★ Guarded by clientMtx, like pendingAudio beside it. */
    std::string preTunedSession;
    /** ★★★ SESSION BY SOCKET, ON EVERY RADIO. `pendingAudio` records the same thing but ONLY when
     *  perClientDsp() is true — which needs a LOCKED CENTRE and maxUsers > 1. A single-listener
     *  receiver has neither, so every per-client structure is dead code there, and two fixes for
     *  the landing bug were written against paths that never ran on the radio reporting it
     *  (2026-08-16). This one is populated at the handshake, for all of them. */
    std::map<net::Socket*, std::string> sockSession;
    std::unique_ptr<vibedsp::Channelizer> chan_;

    // ── ★★★ THE BAND SPECTROGRAM — a 24-hour record, kept by the SERVER ──────────────────────
    //
    // Stuart, 2026-08-05: *"copy UberSDR's spectrogram homework, but make ours rapidly populate
    // so it looks good on the landing page then slow it down, but always keep the previous lines
    // so the oldest data scrolls off the top."*
    //
    // ★★ SERVER-SIDE, NOT PER-BROWSER, and that is the whole point. It must be populated when
    // nobody is connected and survive a reload — a landing page whose history starts empty on
    // every visit is a landing page with nothing to show. One buffer, shared by everyone.
    //
    // ★★★ LOCKED MODE MAKES IT FREE AND MAKES IT HONEST. `BRIEF-band-spectrogram.md` spends its
    // length on one problem: a spectrogram is only readable if every row shares a profile, and
    // inheriting the live dial gives you 40m for an hour then FM then wherever someone left it at
    // 2am. Here the centre CANNOT move — the owner locked it — so the profile is fixed by
    // construction, no row can disagree with another, and taking a row disturbs nobody.
    //
    // ★ TWO RATES, ONE BUFFER. A fresh server has nothing to show, so the first five minutes fill
    // at one row a second; after that it settles to one a minute, which is 24 hours in 1440 rows.
    // Rows are never discarded on the change of pace — the fast ones stay until they age out of
    // the top, so the picture only ever grows more complete.
    struct SpectroRow {
        std::vector<uint8_t> bins;      // dB, same u8 scale as a waterfall row
        int64_t  atMs = 0;              // wall clock, for the time stamps down the side
    };
    // ★★★ RESOLUTION IS THE WHOLE POINT OF ONE OF THESE. At 512 bins over an 8 MHz window each
    //     bin was 15.6 kHz — wider than an AM channel, so every broadcast station smeared into its
    //     neighbours and the picture read as coloured fog. The reference (UberSDR, 0-30 MHz at
    //     ~4096 bins) is 7.3 kHz/bin and you can pick out individual carriers.
    //     ★ The detail is already there and was being thrown away: the shared FFT is 32768 bins
    //       across the capture — 244 Hz/bin at 8 MSPS. Storing 2048 keeps 3.9 kHz/bin, sharper
    //       than the thing we are copying, and costs 1440 x 2048 = ~3 MB of RAM on a box with
    //       gigabytes.
    static constexpr int kSpectroBins = 2048;
    static constexpr int kSpectroRows = 1440;   // 24 h at one row per minute
    static constexpr int kFastRows    = 300;    // the first 5 minutes, at one row per second
    std::mutex               spectroMtx;
    std::deque<SpectroRow>   spectro;

    // ── Persistence ──────────────────────────────────────────────────────────────────────────
    // ★★★ 24 HOURS OF HISTORY MUST NOT DIE WITH THE PROCESS. The landing page's whole appeal is
    //     showing what this receiver has been hearing — and every settings save RESTARTS the
    //     server, so an owner who changed one setting lost the lot. Stuart went to bed expecting a
    //     night of band activity and woke to an empty picture (2026-08-06).
    // ★★ SAVED ON A CLEAN STOP AND EVERY 15 MINUTES. The clean stop covers the common case (a
    //    settings save, a reboot, systemctl restart) at no cost; the periodic write bounds what a
    //    POWER CUT can take to a quarter of an hour. ~3 MB a time is nothing for an SD card at
    //    that rate, and writing every row would be.
    // ★ Format is deliberately the same u8-per-bin the wire uses, with a header carrying the
    //   geometry: a file whose shape does not match this build is DISCARDED rather than
    //   reinterpreted, because a mis-shaped picture is worse than no picture.
    static constexpr uint32_t kSpectroMagic = 0x47505356;   // "VSPG"
    std::string spectroPath;                                 // empty = do not persist
    double      spectroSavedAt = 0;

    void spectroSetPath(const std::string& p) { spectroPath = p; }

    void spectroSave() {
        if (spectroPath.empty()) return;
        std::vector<SpectroRow> rows;
        { std::lock_guard<std::mutex> lk(spectroMtx); rows.assign(spectro.begin(), spectro.end()); }
        if (rows.empty()) return;
        // ★ Temp file beside the target, then rename: a half-written history read at the next
        //   start is worse than none, and rename() cannot cross a filesystem (see the EiBi note).
        const std::string tmp = spectroPath + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return;
        const uint32_t magic = kSpectroMagic;
        const uint32_t bins = kSpectroBins, count = (uint32_t)rows.size();
        const double centre = g_vsLockedCentre.load(), span = sampleRate;
        fwrite(&magic, 4, 1, f); fwrite(&bins, 4, 1, f); fwrite(&count, 4, 1, f);
        fwrite(&centre, sizeof centre, 1, f); fwrite(&span, sizeof span, 1, f);
        for (const auto& r : rows) {
            fwrite(&r.atMs, sizeof r.atMs, 1, f);
            fwrite(r.bins.data(), 1, r.bins.size(), f);
        }
        fclose(f);
        rename(tmp.c_str(), spectroPath.c_str());
        LOGI("spectrogram saved — %zu rows", rows.size());
    }

    void spectroLoad() {
        if (spectroPath.empty()) return;
        FILE* f = fopen(spectroPath.c_str(), "rb");
        if (!f) return;
        uint32_t magic = 0, bins = 0, count = 0;
        double centre = 0, span = 0;
        if (fread(&magic, 4, 1, f) != 1 || magic != kSpectroMagic) { fclose(f); return; }
        if (fread(&bins, 4, 1, f) != 1 || fread(&count, 4, 1, f) != 1) { fclose(f); return; }
        if (fread(&centre, sizeof centre, 1, f) != 1 || fread(&span, sizeof span, 1, f) != 1)
            { fclose(f); return; }
        // ★★ A HISTORY OF A DIFFERENT BAND IS NOT OUR HISTORY. If the owner has moved the window
        //    since, every stored row maps to frequencies that are no longer under those pixels —
        //    so it is discarded rather than drawn against the new axis, which would be a picture
        //    that is confidently wrong.
        if (bins != kSpectroBins || std::fabs(centre - g_vsLockedCentre.load()) > 1.0
            || std::fabs(span - sampleRate) > 1.0) {
            fclose(f);
            LOGI("spectrogram: stored history is for a different window — starting fresh");
            return;
        }
        if (count > (uint32_t)kSpectroRows) count = kSpectroRows;
        std::deque<SpectroRow> in;
        for (uint32_t i = 0; i < count; i++) {
            SpectroRow r;
            r.bins.resize(kSpectroBins);
            if (fread(&r.atMs, sizeof r.atMs, 1, f) != 1) break;
            if (fread(r.bins.data(), 1, kSpectroBins, f) != (size_t)kSpectroBins) break;
            in.push_back(std::move(r));
        }
        fclose(f);
        if (in.empty()) return;
        { std::lock_guard<std::mutex> lk(spectroMtx); spectro = std::move(in); }
        LOGI("spectrogram restored — %zu rows", spectro.size());
    }
    double  spectroAcc[kSpectroBins] = {0};
    /** ★ Set under spectroMtx by the DSP path, ACTED ON elsewhere: a 3 MB write must never
     *  happen on the thread feeding the radio. */
    std::atomic<bool> needSpectroSave{false};
    int     spectroAccN = 0;
    double  spectroLastAt = 0;
    int     spectroTaken  = 0;          // rows so far — decides fast vs slow cadence

    /** Fold one wide FFT row into the spectrogram accumulator, and emit a row when due.
     *  ★ Averaged, not sampled: a single FFT row once a minute would catch whatever happened in
     *  that 40 ms and call it a minute of band activity. Averaging every row between emissions is
     *  what makes a quiet band look quiet instead of speckled. */
    // ── Band conditions, MEASURED HERE ───────────────────────────────────────────────────────
    // ★★★ WHAT THIS RECEIVER CAN ACTUALLY HEAR, which is the half of "band conditions" that solar
    //     indices can never tell you. A prediction models the ionosphere over a region; this is one
    //     aerial in one garden, and for someone deciding whether to listen HERE it is the more
    //     useful number. UberSDR publishes the same idea as `ft8_snr` per band.
    // ★★ FREE, BECAUSE THE FFT IS ALREADY PAID FOR. The FT8 windows for every band inside the
    //    captured span sit in the wide spectrum we compute anyway, so this is a few hundred bin
    //    reads every five seconds — no decoder, no extra DSP, nothing that scales with listeners.
    // ★ FT8 was chosen for the same reason everyone uses it: it is on, worldwide, in a known
    //   3 kHz slot, at all hours. Its slot being busy or empty is a real statement about
    //   propagation into this location.
    struct BandMeas { const char* label; double dialHz; float snrDb; double at; };
    std::vector<BandMeas> bandMeas;
    double bandMeasAt = 0;
    /** ★★★ A ROLLING MEDIAN, NOT THE LAST SAMPLE. One 5-second snapshot of an HF band is mostly
     *  luck: a burst of QRM, a station keying up, or a quiet moment between transmissions moves it
     *  several dB, so the verdict flickered between Fair and Good while nothing about propagation
     *  had changed. Conditions must be LIVE — tracking a real opening within a minute — and
     *  ACCURATE, which a single sample is not (Stuart, 2026-08-06).
     *  ★ MEDIAN rather than mean: the thing being rejected is exactly the occasional loud burst
     *    that a mean would chase. Twelve samples at 5 s is a one-minute window.
     *  ★★ Not persisted, deliberately: unlike the spectrogram this describes RIGHT NOW, and an
     *     hour-old band report restored from disk would be worse than none. */
    static constexpr int kBandWindow = 12;
    std::map<std::string, std::deque<float>> bandHist;

    /** One reading per band whose FT8 slot is inside the captured span. Called from the spectrum
     *  path, which already holds the averaged FFT. */
    void measureBands(const float* sum, int bins, int n) {
        if (bins <= 0 || n <= 0 || g_vsLockedCentre.load() <= 0.0) return;
        const double now = Impl::nowSecs();
        if (now - bandMeasAt < 5.0) return;          // ★ 0.2 Hz: conditions do not change faster
        bandMeasAt = now;

        static const BandMeas kBands[] = {
            {"160m", 1840000, 0, 0}, {"80m",  3573000, 0, 0}, {"60m",  5357000, 0, 0},
            {"40m",  7074000, 0, 0}, {"30m", 10136000, 0, 0}, {"20m", 14074000, 0, 0},
            {"17m", 18100000, 0, 0}, {"15m", 21074000, 0, 0}, {"12m", 24915000, 0, 0},
            {"10m", 28074000, 0, 0},
        };
        const double inv   = 1.0 / (double)n;
        const double binHz = sampleRate / (double)bins;
        const double centre = rtlCenter.load() + hwOffsetHz();
        auto dbAtHz = [&](double hz) -> float {
            const int idx = bins / 2 + (int)llround((hz - centre) / binHz);
            return (idx < 0 || idx >= bins) ? -200.0f : (float)(sum[idx] * inv);
        };

        std::vector<BandMeas> out;
        for (const auto& b : kBands) {
            const double lo = b.dialHz, hi = b.dialHz + 3000.0;    // FT8 occupies dial..dial+3k
            // ★ BOTH EDGES AND THE NOISE REFERENCE MUST BE INSIDE THE CAPTURE. A band half in
            //   view would be measured against bins that do not exist and read as dead.
            // ★★ 0.47, NOT 0.45 — AND THE MEASUREMENT IS WHY THAT IS SAFE. The last of the span is
            //   anti-alias roll-off, where the noise floor slopes; a fixed 0.45 kept clear of it
            //   but also excluded 30m from a 2.5-10.5 MHz window by 24 kHz, so a receiver that
            //   plainly covers the band reported nothing for it. What makes the wider margin
            //   honest is that this is a DIFFERENTIAL reading: the slot and its reference sit
            //   ~20 kHz apart, so a slowly sloping response lifts or drops both together and
            //   largely cancels. An absolute level here would not survive the same move.
            const double edgeLo = centre - sampleRate * 0.47, edgeHi = centre + sampleRate * 0.47;
            if (lo < edgeLo || hi > edgeHi) continue;

            // ★★ THE REFERENCE GOES WHICHEVER SIDE HAS ROOM. Pinning it ABOVE the slot dropped
            //    30m from a 2.5-10.5 MHz window: its FT8 slot at 10.136 is comfortably inside, but
            //    a reference at +12..40 kHz ran past the usable edge, so the band silently
            //    vanished from a receiver that covers it perfectly well (Stuart expected
            //    80/60/40/30 and got 80/60/40). Below is just as good a noise sample.
            double refLo = b.dialHz + 12000.0, refHi = b.dialHz + 40000.0;
            if (refHi > edgeHi) { refLo = b.dialHz - 40000.0; refHi = b.dialHz - 12000.0; }
            if (refLo < edgeLo) continue;                  // no room either side: say nothing

            std::vector<float> sig, ref;
            for (double f = lo; f <= hi; f += binHz)       sig.push_back(dbAtHz(f));
            for (double f = refLo; f <= refHi; f += binHz) ref.push_back(dbAtHz(f));
            if (sig.size() < 4 || ref.size() < 8) continue;
            std::sort(sig.begin(), sig.end());
            std::sort(ref.begin(), ref.end());
            // ★ A HIGH PERCENTILE OF THE SLOT against a LOW percentile of its neighbourhood: the
            //   slot's peak is the signals in it, the neighbourhood's floor is the noise. Using a
            //   mean either side would let one strong carrier next door flatter a dead band.
            const float s90 = sig[(size_t)(sig.size() * 0.90)];
            const float r10 = ref[(size_t)(ref.size() * 0.10)];
            auto& h = bandHist[b.label];
            h.push_back(s90 - r10);
            while ((int)h.size() > kBandWindow) h.pop_front();
            std::vector<float> w(h.begin(), h.end());
            std::sort(w.begin(), w.end());
            BandMeas m = b;
            m.snrDb = w[w.size() / 2];
            m.at    = now;
            out.push_back(m);
        }
        std::lock_guard<std::mutex> lk(clientMtx);
        bandMeas.swap(out);
    }

    /** JSON array of what we measured. Empty when the captured span holds no FT8 slot — which is
     *  the correct answer on an FM profile, and must read as "nothing to say" rather than "zero". */
    std::string bandMeasJson() {
        std::lock_guard<std::mutex> lk(clientMtx);
        std::string j = "[";
        for (size_t i = 0; i < bandMeas.size(); i++) {
            char b[128];
            snprintf(b, sizeof b, "%s{\"band\":\"%s\",\"snrDb\":%.1f}",
                     i ? "," : "", bandMeas[i].label, bandMeas[i].snrDb);
            j += b;
        }
        return j + "]";
    }

    void spectroFeed(const float* sum, int bins, int n) {
        if (bins <= 0 || n <= 0 || g_vsLockedCentre.load() <= 0.0) return;   // fixed profile only
        // ★ fftAccum holds SUMS over FFT_AVG frames, not averages — the caller divides at its own
        //   emit site. Divide here too, or every row is offset by 10*log10(FFT_AVG) and the whole
        //   image sits at the wrong level.
        const double inv = 1.0 / (double)n;
        for (int i = 0; i < kSpectroBins; i++) {
            // Peak-hold across the group: a narrow carrier must survive the downsample, which is
            // the whole reason anyone reads one of these.
            const int lo = (int)((int64_t)i * bins / kSpectroBins);
            const int hi = (int)((int64_t)(i + 1) * bins / kSpectroBins);
            float best = -300.0f;
            for (int j = lo; j < hi && j < bins; j++) { const float v = (float)(sum[j] * inv); if (v > best) best = v; }
            spectroAcc[i] += best;
        }
        spectroAccN++;

        const double now = nowSecs();
        const double period = spectroTaken < kFastRows ? 1.0 : 60.0;
        if (spectroLastAt == 0) spectroLastAt = now;
        if (now - spectroLastAt < period) return;
        spectroLastAt = now;

        SpectroRow r;
        r.bins.resize(kSpectroBins);
        for (int i = 0; i < kSpectroBins; i++) {
            const double v = spectroAcc[i] / std::max(1, spectroAccN);
            const int q = (int)lround(v + 256.0);
            r.bins[i] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
            spectroAcc[i] = 0;
        }
        spectroAccN = 0;
        r.atMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(spectroMtx);
        spectro.push_back(std::move(r));
        // ★ Oldest scrolls off the top, exactly as asked — the buffer never grows without bound.
        while ((int)spectro.size() > kSpectroRows) spectro.pop_front();
        // ★ Driven from the feed rather than a timer: no extra thread, and it cannot fire while
        //   there is nothing new to write.
        const double nowS = Impl::nowSecs();
        if (spectroSavedAt <= 0) spectroSavedAt = nowS;
        else if (nowS - spectroSavedAt > 900.0) { spectroSavedAt = nowS; needSpectroSave = true; }
        spectroTaken++;
    }

    /** ★ The gate. Per-client DSP exists only where it means something: a SHARED receiver, where
     *  the owner has locked the centre and more than one listener is allowed. */
    bool perClientDsp() const {
        return g_vsLockedCentre.load() > 0.0 && g_vsMaxUsers.load() > 1;
    }

    /** Narrowest channel that still carries this mode, as a power of two dividing fftSize.
     *  ★ The channel must be comfortably wider than the demodulator's passband — a channel cut to
     *  exactly the filter width puts the filter skirts on the channel edge, where the fast
     *  convolution's own transition lives. 2.5x keeps them apart. */
    int chanBinsFor(double bwHz) const {
        const double need = std::max(bwHz * 2.5, 24000.0);
        int b = 64;
        while (b < fftSize && sampleRate * (double)b / (double)fftSize < need) b <<= 1;
        return std::min(b, fftSize);
    }

    /** This listener's own zoomed waterfall row. Same wire format as the shared one — the client
     *  needs no new code path, it simply sees a sharper picture than the person next to it. */
    static void clientZoomCb(void* ctx, const float* db, int bins) {
        auto* c = (ClientDsp*)ctx;
        if (c && c->owner) c->owner->onClientZoom(c, db, bins);
    }

    static void clientAudioCb(void* ctx, const float* pcm, int frames, int ch, int) {
        auto* c = (ClientDsp*)ctx;
        if (!c || !c->owner || frames <= 0) return;
        c->owner->onClientAudio(c, pcm, frames, ch);
    }

    /** ★★★ RDS AND THE STEREO PILOT COME FROM A LISTENER'S OWN PIPELINE NOW. They were wired to
     *  the SHARED one only, which in per-client mode demodulates for nobody — so RDS, Advanced RDS
     *  and the stereo indicator were all dead on a shared receiver, while the audio itself was
     *  perfectly fine (Stuart, 2026-08-05: "RDS and ADVANCED RDS not working"... "oh no it IS in
     *  stereo, just not displaying the icon" — which is the tell: the DEMODULATOR was working and
     *  only the telemetry hanging off it was not).
     *  ★ FIFTH instance of the family in one session. The rule earns its keep: when a shared
     *    resource becomes per-listener, every path that identified the listener must be re-derived.
     *
     *  ★★ ONE OWNER AT A TIME, because the RDS state it feeds is a single set of fields on Impl —
     *  including the whole Advanced RDS surface. Two WFM listeners writing it at once would
     *  interleave two stations' names into one. The first WFM listener claims it; when they leave
     *  WFM or disconnect the claim is released and the next one takes it.
     *  ▶ Genuinely per-listener RDS means moving ~30 fields onto ClientDsp. Worth doing, not
     *    tonight, and the same known limitation the decoders already carry. */
    /** ★★★ EVERY LISTENER DECODES THEIR OWN. Each forwards into that listener's OWN RdsState, so
     *  user 1 on Heart and user 2 on Radio 1 see their own station's name, PI, RadioText and
     *  stereo flag — which is what an OWRX-style profile means and what the earlier
     *  single-owner design could not do (Stuart, 2026-08-05). */
    static void cRdsPs(void* ctx, uint16_t pi, const char* ps8) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) rdsPsCb_(c->rdsS, c->vfoHz, c->owner, pi, ps8); }
    static void cRdsText(void* ctx, const char* rt64) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) rdsTextCb_(c->rdsS, c->vfoHz, c->owner, rt64); }
    static void cRdsPi(void* ctx, uint16_t pi) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) rdsPiCb_(c->rdsS, c->vfoHz, c->owner, pi); }
    static void cRdsExt(void* ctx, const vibedsp::RxPipeline::Callbacks::RdsExt& x) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) rdsExtCb_(c->rdsS, c->vfoHz, c->owner, x); }
    static void cRdsSig(void* ctx, float relDb) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) rdsSigCb_(c->rdsS, c->vfoHz, c->owner, relDb); }
    static void cRdsBer(void* ctx, int percent) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) rdsBerCb_(c->rdsS, c->vfoHz, c->owner, percent); }
    static void cRdsEcc(void* ctx, uint8_t ecc) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) rdsEccCb_(c->rdsS, c->vfoHz, c->owner, ecc); }
    static void cStereo(void* ctx, bool locked) {
        auto* c = (ClientDsp*)ctx; if (c && c->owner) stereoCb_(c->rdsS, c->vfoHz, c->owner, locked); }

    static void freeClientFx(ClientDsp* c) {
        std::lock_guard<std::mutex> lk(c->fxMtx);
        delete c->nrEng;    c->nrEng = nullptr;
        delete c->notchEng; c->notchEng = nullptr;
    }

    void clientThread(std::shared_ptr<ClientDsp> c) {
        vibeAudioThread("vibe-listener");
        for (;;) {
            std::shared_ptr<const ClientDsp::Block> blk;
            {
                std::unique_lock<std::mutex> lk(c->qm);
                c->qcv.wait(lk, [&]{ return !c->q.empty() || !c->run.load(); });
                if (!c->run.load()) return;
                blk = std::move(c->q.front());
                c->q.pop_front();
            }
            // ★ Time the work this listener costs. Measured around feedOneClient only: that IS
            //   this listener's private pipeline (channelise, demodulate, decode, encode), and
            //   nothing shared is inside it.
            // ★★★ CPU TIME, NOT WALL TIME. steady_clock measured ELAPSED time, which includes
            //     every moment this thread sat descheduled — so under load the figure inflated
            //     with the machine's own contention rather than with this listener's work. A
            //     stress test made that plain: an Airspy doing exactly one WFM stream read 17%
            //     idle and 106% while the other radios were hammered, having done no more work at
            //     all (2026-08-08). A capacity number that rises when OTHER people arrive is worse
            //     than none, because it is the number an owner sizes their server with.
            timespec c0{}, c1{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c0);
            feedOneClient(c, blk->bins.data(), blk->index);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1);
            c->dspNanos.fetch_add(
                (unsigned long long)((c1.tv_sec - c0.tv_sec) * 1000000000LL
                                     + (c1.tv_nsec - c0.tv_nsec)),
                std::memory_order_relaxed);
        }
    }

    void startClientThread(const std::shared_ptr<ClientDsp>& c) {
        c->th = std::thread([this, c]{ clientThread(c); });
    }

    void stopClientThread(const std::shared_ptr<ClientDsp>& c) {
        { std::lock_guard<std::mutex> lk(c->qm); c->run.store(false); }
        c->qcv.notify_all();
        if (c->th.joinable()) c->th.join();
        freeClientFx(c.get());        // ★ after the thread has stopped touching them
    }

    /** Decide whether this listener decodes RDS for its own frequency, and tell its pipeline.
     *  ★ Called on every retune, so ownership follows the dial. Cheap: a map lookup and an atomic.
     *  Call WITHOUT clientMtx held. */
    /** Recompute EVERY listener's RDS decode assignment: one owner per occupied frequency, and
     *  nobody left decoding a frequency they have moved off.
     *  ★★★ WHY A FULL RESWEEP AND NOT AN INCREMENTAL CLAIM. A claim that only ever adjusts the
     *      CALLER strands everyone else: two listeners both land on the server's landing
     *      frequency, the first claims it, the second is refused — and when they later tune apart
     *      the loser never re-claims, because its own retune has already been and gone. It
     *      decodes nothing for the rest of its session while `owners=1` looks perfectly healthy.
     *      Measured exactly that way on the Pi.
     *      ★ The set is at most one entry per listener and this runs on retune, not per block, so
     *        the cost is irrelevant next to being correct.
     *  Call WITHOUT clientMtx held. */
    void rdsResweep() {
        std::vector<std::pair<std::shared_ptr<ClientDsp>, bool>> decisions;
        {
            std::lock_guard<std::mutex> lk(clientMtx);
            rdsFreqOwner.clear();
            for (auto& kv : clientDsp) {
                auto& d = kv.second;
                bool decode = false;
                if (d->mode == "wfm") {
                    const long long key = rdsKeyOf(d->vfoHz);
                    auto it = rdsFreqOwner.find(key);
                    if (it == rdsFreqOwner.end()) { rdsFreqOwner[key] = d.get(); decode = true; }
                }
                decisions.emplace_back(d, decode);
            }
        }
        // ★ Applied outside the lock: setRdsEnabled touches the pipeline, and holding clientMtx
        //   across DSP calls is how this file has deadlocked itself before.
        for (auto& [d, decode] : decisions) {
            d->rdsDecode = decode;
            if (d->rx) d->rx->setRdsEnabled(decode);
        }
    }

    /** The listener currently decoding RDS for `c`'s frequency — `c` itself, or whoever got there
     *  first. Null when nobody on this frequency is decoding (nothing to report yet).
     *  Call WITHOUT clientMtx held; returns a shared_ptr so the state cannot die while it is read. */
    std::shared_ptr<ClientDsp> rdsSourceFor(const std::shared_ptr<ClientDsp>& c) {
        if (!c) return nullptr;
        if (c->rdsDecode) return c;
        std::lock_guard<std::mutex> lk(clientMtx);
        auto it = rdsFreqOwner.find(rdsKeyOf(c->vfoHz));
        if (it == rdsFreqOwner.end()) return nullptr;
        // ★ Validate on read as well: an owner that has retuned since claiming is decoding a
        //   DIFFERENT station, and returning its state would put someone else's name on this
        //   listener's bar — the exact staleness this whole area keeps producing.
        for (auto& kv : clientDsp)
            if (kv.second.get() == it->second)
                return (kv.second->mode == "wfm" && rdsKeyOf(kv.second->vfoHz) == it->first)
                     ? kv.second : nullptr;
        return nullptr;
    }

    void clientRetune(ClientDsp* c) {
        if (!c) return;
        // ★★★ RDS IDENTIFIES ONE CARRIER, SO IT DIES WITH THE DIAL. Clear it whenever this
        //     listener's frequency or mode changes — the shared retune path has always done this
        //     (see retune()), and the per-client one did not, so a listener who tuned away kept
        //     the previous station's NAME and PI while their own decoder reported nothing:
        //     ps="Heart" alongside ber=-1 and sig=-99, which reads as "RDS is shared and broken"
        //     when it is in fact per-listener and merely stale. Measured on the Pi, 2026-08-05.
        //     ★ It is the same rule the WEB client applies at its end (expireRdsIfRetuned) — a
        //       station name outliving the tune is the bug both were written to prevent.
        if (c->mode != "wfm" || c->vfoHz != c->rdsAtHz) {
            c->rdsAtHz = c->vfoHz;
            std::lock_guard<std::mutex> lk(c->rdsS.rdsMtx);
            c->rdsS.rdsPsName.clear(); c->rdsS.rdsText.clear();
            c->rdsS.rdsPi = -1; c->rdsS.rdsEcc = 0; c->rdsS.rdsBer = -1; c->rdsS.rdsSig = -99.0f;
            c->rdsS.stereoDetected.store(false);
            // ★★★ HAVING CLEARED IT, ASK FOR IT BACK. The pipeline reports stereo on a CHANGE,
            //     and a retune inside WFM does not rebuild the chain, so the pilot stays locked
            //     and there is no change to report — this clear would otherwise be permanent, and
            //     the listener sat in mono on a full-strength stereo station until they did
            //     something that rebuilt the chain (Stuart, 2026-08-08: "every time I tune it
            //     requires the advanced RDS box to be closed and opened again").
            if (c->rx) c->rx->requestStereoReport();
            // ★ And drop what the decoder learned from the PREVIOUS station — its timing hypothesis
            //   scores outlive a retune, and a stale one beats the correct one on a weak signal.
            if (c->rx) c->rx->requestRdsResync();
        }
        const auto mp = paramsFor(c->mode);
        const double bw = c->bwHz > 0 ? c->bwHz : mp.bandwidth;
        // ★★★ THE CHANNEL IS SIZED BY THE DEMODULATOR, AND BY NOTHING ELSE.
        //     It used to be sized by max(audio, view) so one channel could serve both. That is
        //     tidy and it is wrong: changing the ZOOM then changed the channel WIDTH, which
        //     rebuilds the pipeline — and a rebuilt pipeline restarts its AGC and filter state,
        //     so the audio ducked every time somebody zoomed (Stuart, 2026-08-05: "the zoom is
        //     attenuating the audio, tune is fine though" — tuning kept the same width, which is
        //     exactly why it was fine).
        //     ★★ Same bug as memory/tuning_attenuates_agc_reset.md, one field along: THE THING
        //        THAT MUST NOT REBUILD THE AUDIO CHAIN IS ANYTHING THE LISTENER TOUCHES OFTEN.
        //     ★ The view is served from this channel when it fits inside it — which is every zoom
        //       deep enough to be worth a private view — and from the shared wide row when it does
        //       not. That is the same handover, decided without resizing anything.
        const int want = chanBinsFor(bw);
        std::lock_guard<std::mutex> lk(c->mtx);
        if (want != c->chanBins || !c->rx) {
            c->chanBins = want;
            c->chanRate = sampleRate * (double)want / (double)fftSize;
            c->slice.assign((size_t)want, cf32{0.0f, 0.0f});
            c->rx.reset(new vibedsp::RxPipeline());
            vibedsp::RxPipeline::Callbacks cb{};
            cb.ctx = c;
            cb.audio = &Impl::clientAudioCb;
            cb.zoomSpectrum = &Impl::clientZoomCb;
            // ★ Always wired; the forwarders themselves check ownership, so a listener who becomes
            //   the RDS owner later does not need their pipeline rebuilding to start reporting.
            cb.rdsPs = &Impl::cRdsPs;   cb.rdsPi   = &Impl::cRdsPi;
            cb.rdsBer = &Impl::cRdsBer; cb.rdsSig  = &Impl::cRdsSig;
            cb.rdsExt = &Impl::cRdsExt; cb.rdsText = &Impl::cRdsText;
            cb.rdsEcc = &Impl::cRdsEcc; cb.stereo  = &Impl::cStereo;
            // ★ A small FFT: this pipeline exists to DEMODULATE. The waterfall everyone sees is
            //   the shared wide one, so paying for a per-client spectrum here would be paying
            //   twice for a picture nobody reads.
            c->rx->start(c->chanRate, 1024, 10.0, (int)AUDIO_SR, cb);
            // ★★★ RE-APPLY THIS LISTENER'S SETTINGS TO THE NEW PIPELINE. A width change builds a
            //     fresh RxPipeline with fresh defaults, so anything the listener had chosen would
            //     be silently forgotten the next time they changed mode — the control still ON in
            //     their UI and no longer doing anything, which is the WORST version of this bug
            //     because it looks like it is working. Same reasoning as the RSP front end being
            //     re-applied after the AGC kick: whatever rebuilds must restore.
            if (c->deempTau >= 0) c->rx->setDeemphasis(c->deempTau);
            c->rx->setStereoEnabled(c->stereoOn.load());
            // ★ And the four FM treatments, for exactly the reason above. Adding a per-listener
            //   control without adding it here is how it comes to look like it is working.
            c->rx->setWeakSignalProc(c->wspOn.load());
            c->rx->setIms(c->imsOn.load());
            c->rx->setCeq(c->ceqOn.load());
            c->rx->setNoiseBlanker(c->nbOn.load());
            LOGI("client channel: %.3f kHz wide (%d bins) for %s",
                 c->chanRate / 1e3, want, c->mode.c_str());
        }
        // ★★★ THE RESIDUAL IS NOT OPTIONAL. extract() centres on a whole BIN, so the channel
        //     lands up to half a bin away from where the listener actually tuned — 122 Hz at
        //     8 MSPS with a 32k FFT, which on SSB is the difference between speech and a growl.
        //     Hand the leftover to the pipeline, which tunes within the channel.
        const double binHz = sampleRate / (double)fftSize;
        const double off   = c->vfoHz - rtlCenter.load() - hwOffsetHz();
        const double resid = off - std::lround(off / binHz) * binHz;
        c->rx->setTune(resid, rxModeFor(mp.kind), bw);
        // ★ After the pipeline exists, so setRdsEnabled has something to act on. A FULL resweep,
        //   because this listener moving can free a frequency somebody else is sitting on — see
        //   rdsResweep().
        rdsResweep();
        // ★ The view centre expressed INSIDE this listener's channel. The channel is centred on
        //   its VFO, so the view offset is the gap between where it is listening and where it is
        //   looking — they are not the same thing once you can pan away from the signal.
        clientRetuneView(c);
        // ★ Stamp what this cut was made against, so a later hardware move is detectable — see
        //   ClientDsp::tunedAtCentre.
        c->tunedAtCentre = rtlCenter.load();
    }

    /** This listener's VIEW channel — a second, independent extract sized for what it is LOOKING
     *  at rather than what it is listening to.
     *  ★ Free to rebuild on every zoom: it produces no audio, so restarting its filters costs a
     *    frame, not a duck. That separation is the whole point of having two. */
    void clientRetuneView(ClientDsp* c) {
        if (!c->ownView || c->viewSpanHz <= 0) {
            if (c->viewRx) { c->viewRx.reset(); c->viewChanBins = 0; }
            return;
        }
        // ★★★ ONE FIXED WIDTH FOR EVERY ZOOM LEVEL — the view channel must NOT be resized as the
        //     listener zooms. Sizing it to each view meant a new width every time a zoom crossed a
        //     power-of-two boundary, and a new width rebuilds the pipeline: the fresh ZoomSpectrum
        //     has to refill its accumulator before it emits, so the waterfall STOPPED for a moment
        //     on the way in (Stuart, 2026-08-05: "there is a hitch moving into the most zoomed in
        //     levels where the spectrum stops for a split second"). Zooming is the single most
        //     interactive thing on the page; it must not rebuild anything.
        //     ★★ Same lesson as the audio channel this morning, arrived at from the other side:
        //        THE THING A LISTENER TOUCHES CONSTANTLY MUST NOT REBUILD A PIPELINE. There it
        //        cost a duck in the audio; here a gap in the picture.
        //     ★ Width = the handover span, which is the widest view this path ever serves. Below
        //       it, RxPipeline's own ZoomSpectrum decimates to whatever span is asked for — that
        //       is exactly what it is for, and it costs nothing to ask it for a narrower one.
        // ★★★ SIZED TO THE VIEW, NOT TO THE HANDOVER. Pinning it at the handover width (1 MHz at
        //     8 MSPS) to dodge the rebuild hitch made every listener run a 1 MHz pipeline instead
        //     of a ~30 kHz one — measured at 36-42% of real time for a SINGLE listener, so thirty
        //     would have needed twelve times real time. It is what destroyed the 30-user test.
        //     ★★ And it bought nothing: the rebuild hitch is already covered by `viewPriming`,
        //        which keeps the listener on the shared row until the new view is ready. That was
        //        added AFTER this, and it is the fix — this was the scaffolding, left standing.
        //     ★ Lesson: when a later fix subsumes an earlier workaround, take the workaround OUT.
        //       Two fixes for one problem is how a cheap operation becomes a 30x one.
        const int want = chanBinsFor(c->viewSpanHz * 1.25);
        if (want != c->viewChanBins || !c->viewRx) {
            c->viewChanBins = want;
            c->viewChanRate = sampleRate * (double)want / (double)fftSize;
            c->viewSlice.assign((size_t)want, cf32{0.0f, 0.0f});
            c->viewRx.reset(new vibedsp::RxPipeline());
            vibedsp::RxPipeline::Callbacks vcb{};
            vcb.ctx = c;
            vcb.zoomSpectrum = &Impl::clientZoomCb;
            // ★★ THIS LISTENER'S OWN RATE, not the engine's. A listener drawing its own zoomed
            //    view is fed by its own pipeline, so its rate is set here rather than decimated on
            //    the way out — and passing the engine rate (the FASTEST listener's) would have
            //    handed an idle phone the full 20 fps it had just asked to be spared.
            c->viewRx->start(c->viewChanRate, 1024, fpsFor(c->spec), (int)AUDIO_SR, vcb);
            LOGI("client view channel: %.3f kHz wide (%d bins) for a %.3f kHz view",
                 c->viewChanRate / 1e3, want, c->viewSpanHz / 1e3);
        }
        // The slice is centred on the VIEW centre bin, so the zoom sits at the residual only.
        const double binHz = sampleRate / (double)fftSize;
        const double off   = c->viewCentreHz - rtlCenter.load() - hwOffsetHz();
        const double resid = off - std::lround(off / binHz) * binHz;
        c->viewRx->setZoomBins(binsFor(c->spec));
        c->viewRx->setZoomView(resid, c->viewSpanHz, fpsFor(c->spec));
        c->viewPriming.store(true);      // cleared by the first frame out of the new view
    }

    /** The signed centre bin this listener's VIEW channel is taken from. */
    int clientViewCentreBin(const ClientDsp* c) const {
        const double binHz = sampleRate / (double)fftSize;
        return (int)std::lround((c->viewCentreHz - rtlCenter.load() - hwOffsetHz()) / binHz);
    }

    /** The signed centre bin this listener's channel is taken from. */
    int clientCentreBin(const ClientDsp* c) const {
        const double binHz = sampleRate / (double)fftSize;
        return (int)std::lround((c->vfoHz - rtlCenter.load() - hwOffsetHz()) / binHz);
    }

    /** Frame and send this listener's own zoomed row. Mirrors onZoomSpectrum's un-shift exactly:
     *  the wire puts DC at bin 0, while the zoom FFT hands back a shifted row. */
    void onClientZoom(ClientDsp* c, const float* db, int nb) {
        c->viewPriming.store(false);     // the sharp path is live — the shared row can stand down
        std::shared_ptr<net::Socket> sock = c->spec;
        if (!sock || !sock->isOpen() || nb <= 0) return;
        const int outBins = nb;
        std::vector<uint8_t> frame(22 + outBins);
        frame[0]='S';frame[1]='P';frame[2]='E';frame[3]='C';frame[4]=0x01;frame[5]=0x03;
        uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::memcpy(&frame[6], &ts, 8);
        uint64_t f = (uint64_t)llround(c->viewCentreHz);
        std::memcpy(&frame[14], &f, 8);
        const int half = outBins / 2;
        for (int i = 0; i < outBins; i++) {
            const int signedOut = (i <= half) ? i : i - outBins;
            int src = half + signedOut;
            if (src < 0) src = 0; else if (src >= nb) src = nb - 1;
            int v = (int)lround(db[src] + 256.0);
            frame[22+i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        sendWs(sock, 0x2, frame.data(), frame.size(), Out::Spectrum);
        vsSpecBytes.fetch_add(frame.size(), std::memory_order_relaxed);
    }

    /** Encode and send one block of this listener's audio to ITS OWN socket. */
    void onClientAudio(ClientDsp* c, const float* pcm, int frames, int ch) {
        // ★★★ THE DECODERS RUN FIRST, AND WITHOUT AN AUDIO SOCKET.
        //     This used to sit BELOW the `if (!sock || !sock->isOpen()) return;` guard, so in
        //     per-client mode the decoders only ran while the owning listener happened to have an
        //     open audio WebSocket — and stopped, silently, the instant it blipped. The shared
        //     path has always promised the opposite in its own comment: "runs even with no audio
        //     WS client". Per-client DSP quietly dropped that guarantee.
        //     ★★ WHAT IT LOOKS LIKE FROM OUTSIDE: WEFAX misses an ENTIRE transmission (one blip
        //     inside a 10-20 minute image loses the lot), and RTTY prints a perfect line, then a
        //     corrupted one, then a perfect one — which reads as poor reception, not as a gap in
        //     the feed. Stuart, 2026-08-06: "the RTTY signal is strong but it seems to be garbling
        //     more than usual… WEFAX completely missed an entire transmission too." The second
        //     symptom is what ruled out the front end: an AGC degrades an image, it does not
        //     delete one.
        //     ★ A decoder is not a listener. It needs the SAMPLES, and nothing about whether
        //       anyone is listening to them — which is exactly why the audio socket must not be
        //       able to gate it.
        //   Guarded on decoderAttached so that with no decoder attached — the usual case — this
        //   costs an atomic read rather than clientMtx and a map scan on every audio block.
        if (decoderAttached.load(std::memory_order_relaxed)) {
            auto owner = decoderOwner();
            if (owner && owner.get() == c) {
                std::vector<stereo_t> st((size_t)frames);
                for (int i = 0; i < frames; i++) {
                    st[i].l = pcm[i * ch];
                    st[i].r = ch == 2 ? pcm[i * ch + 1] : pcm[i * ch];
                }
                feedDecoder(st.data(), frames);
                feedSpots(st.data(), frames);
                decoderFedSamples.fetch_add((uint64_t)frames, std::memory_order_relaxed);
            }
        }
        std::shared_ptr<net::Socket> sock;
        { std::lock_guard<std::mutex> lk(clientMtx); sock = c->audio; }
        if (!sock || !sock->isOpen()) return;
        // ── This listener's own effects ──────────────────────────────────────────────────────
        // ★ Mono only, exactly as the shared path has always been: the auto notch tracks a single
        //   tone and NR's spectral subtraction is built for one channel. On a stereo WFM listen
        //   they are simply not applied, which is what the client's own gating already assumes.
        std::vector<float> fx;
        const float* out = pcm;
        int outFrames = frames;
        if (ch == 1 && (c->notchOn.load() || c->nrOn.load())) {
            fx.assign(pcm, pcm + frames);
            std::lock_guard<std::mutex> lk(c->fxMtx);
            // Notch BEFORE NR: it removes steady carriers, which NR would otherwise learn as
            // part of the noise floor and then preserve.
            if (c->notchOn.load()) {
                if (!c->notchEng) c->notchEng = new AutoNotch();
                c->notchEng->process(fx.data(), (int)fx.size());
            }
            if (c->nrOn.load()) {
                if (!c->nrEng) { c->nrEng = new AudioNR(); c->nrEng->setStrength(c->nrStrength); }
                std::vector<float> nrOut;
                c->nrEng->process(fx.data(), (int)fx.size(), nrOut);
                // ★ STFT latency: the first frames produce NOTHING. Returning early is correct —
                //   sending the un-processed audio instead would make switching NR on emit a
                //   moment of raw audio, which reads as NR "not doing anything".
                if (nrOut.empty()) return;
                fx.swap(nrOut);
            }
            out = fx.data();
            outFrames = (int)fx.size();
        }

        std::vector<int16_t> buf((size_t)outFrames * ch);
        for (int i = 0; i < outFrames * ch; i++) {
            float v = out[i];
            v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
            buf[i] = (int16_t)std::lround(v * 32767.0f);
        }
        sendClientAudio(c, sock, buf.data(), outFrames, ch);
    }

    struct SpecPeer { std::shared_ptr<net::Socket> sock; int bins; double fps; };

    /** Everyone receiving spectrum right now, WITH the width each asked for — primary first.
     *  Call WITHOUT clientMtx held. */
    std::vector<SpecPeer> allSpecPeers() {
        std::vector<SpecPeer> out;
        std::lock_guard<std::mutex> lk(clientMtx);
        auto add = [&](const std::shared_ptr<net::Socket>& s) {
            auto it = clientBins.find(s.get());
            auto fit = clientFps.find(s.get());
            out.push_back({s, it == clientBins.end() ? WIRE_BINS_DEFAULT : it->second,
                           fit == clientFps.end() ? 0.0 : fit->second});
        };
        if (specClient && specClient->isOpen()) add(specClient);
        for (auto& s : specExtra) if (s && s->isOpen()) add(s);
        return out;
    }
    /** Everyone receiving spectrum right now — for paths that do not care about width. */
    std::vector<std::shared_ptr<net::Socket>> allSpecClients() {
        std::vector<std::shared_ptr<net::Socket>> out;
        for (auto& p : allSpecPeers()) out.push_back(p.sock);
        return out;
    }
    /** ★★★ THE ENGINE RUNS AT THE FASTEST RATE ANY LISTENER WANTS — never at the slowest.
     *  Mirrors wireBins() directly above, and for the same reason: a listener asking for less can
     *  always be given less on the way out, but a listener asking for more cannot be given frames
     *  that were never computed. Running at the min is what let one idle phone slow everybody.
     *  ★ The owner's configured rate is the FLOOR, not a starting point to be lost: with nobody
     *    asking, the radio sits at --fps, and when the last slow listener leaves it returns there
     *    rather than staying wherever that listener left it.
     *  Call WITHOUT clientMtx held. */
    void recomputeEngineRate() {
        double want = 0.0;
        {
            std::lock_guard<std::mutex> lk(clientMtx);
            auto acc = [&](const std::shared_ptr<net::Socket>& s) {
                auto it = clientFps.find(s.get());
                // A listener that has never asked wants the server default, not zero — otherwise
                // a single silent client would hold the max at 0 and fall through to the floor.
                want = std::max(want, it == clientFps.end() || it->second <= 0 ? baseFftRate : it->second);
            };
            if (specClient && specClient->isOpen()) acc(specClient);
            for (auto& s : specExtra) if (s && s->isOpen()) acc(s);
        }
        if (want <= 0) want = baseFftRate;
        const double mr = g_vsMaxFftRate.load();     // the owner's ceiling still wins
        if (mr > 0 && want > mr) want = mr;
        if (want <= 0 || std::fabs(want - fftRate) < 0.01) return;
        applyEngineRate(want);
    }

    /** Set the engine rate itself. The per-client decimation below turns this into each
     *  listener's own rate; nothing else should call rx.setFftRate directly. */
    void applyEngineRate(double fps) {
        fftRate = fps;
        // The engine runs at FFT_AVG× the EMIT rate — onSpectrum block-averages FFT_AVG frames
        // into each one it sends. Pass the raw fps and everything comes out 4× too slow.
        rx.setFftRate(fps * FFT_AVG);
        // ★★ THE ZOOM PATH HAS ITS OWN RATE and only updateZoomView() sets it — a rate change that
        //    reached one path and not the other needed a PAGE REFRESH to take effect.
        updateZoomView();
        LOGI("engine fft rate: %.1f fps (engine %.1f) — the fastest listener's rate", fps, fps * FFT_AVG);
    }

    /** Who gets THIS frame. Advances each listener's phase accumulator by its share of the engine
     *  rate and returns one flag per peer, in peer order.
     *  ★★ CALL EXACTLY ONCE PER EMITTED FRAME, and only from the path that is actually emitting.
     *     The wide path and the zoom path suppress each other; advancing in both would double
     *     every accumulator and hand everyone twice the rate they asked for.
     *  Call WITHOUT clientMtx held. */
    std::vector<char> dueForFrame(const std::vector<SpecPeer>& peers) {
        std::vector<char> due(peers.size(), 1);
        const double engine = fftRate;
        if (engine <= 0) return due;
        std::lock_guard<std::mutex> lk(clientMtx);
        for (size_t i = 0; i < peers.size(); i++) {
            const double want = peers[i].fps > 0 ? peers[i].fps : baseFftRate;
            if (want <= 0 || want >= engine) { clientFpsAcc[peers[i].sock.get()] = 0.0; continue; }
            double& a = clientFpsAcc[peers[i].sock.get()];
            a += want / engine;
            if (a >= 1.0) { a -= 1.0; due[i] = 1; } else due[i] = 0;
            // Never let the accumulator run away if the engine rate drops under us — a stored
            // surplus would come back out as a burst of frames the listener did not ask for.
            if (a > 1.0) a = 1.0;
        }
        return due;
    }

    /** This client's requested rate, or the server default if it has never asked.
     *  Call WITHOUT clientMtx held. */
    double fpsFor(const std::shared_ptr<net::Socket>& s) {
        if (!s) return baseFftRate;
        std::lock_guard<std::mutex> lk(clientMtx);
        auto it = clientFps.find(s.get());
        return (it == clientFps.end() || it->second <= 0) ? baseFftRate : it->second;
    }

    /** This client's requested width. Call WITHOUT clientMtx held. */
    int binsFor(const std::shared_ptr<net::Socket>& s) {
        if (!s) return WIRE_BINS_DEFAULT;
        std::lock_guard<std::mutex> lk(clientMtx);
        auto it = clientBins.find(s.get());
        return it == clientBins.end() ? WIRE_BINS_DEFAULT : it->second;
    }
    /** ★★ The width the DSP-side ZOOM FFT runs at: the WIDEST any listener wants.
     *  The zoom path makes real bins in the DSP, so unlike the wide path it cannot just be rebuilt
     *  per client — but a client wanting fewer can always be peak-held DOWN from a wider row.
     *  Running at the max serves everyone honestly; running at the min would hand the widest
     *  listener interpolated mush. */
    int wireBins() {
        std::lock_guard<std::mutex> lk(clientMtx);
        int n = 0;
        auto acc = [&](const std::shared_ptr<net::Socket>& s) {
            auto it = clientBins.find(s.get());
            n = std::max(n, it == clientBins.end() ? WIRE_BINS_DEFAULT : it->second);
        };
        if (specClient && specClient->isOpen()) acc(specClient);
        for (auto& s : specExtra) if (s && s->isOpen()) acc(s);
        return n > 0 ? n : WIRE_BINS_DEFAULT;
    }
    /** How many listeners are attached (spectrum), with clientMtx ALREADY HELD.
     *  ★★★ THIS SPLIT IS NOT TIDINESS — IT IS THE FIX FOR A HARD DEADLOCK (2026-08-04).
     *  `acceptWs` evaluates the "is the server full?" test INSIDE its clientMtx scope, and that
     *  test called specListenerCount(), which locks clientMtx again. std::mutex is NOT recursive,
     *  so the second listener deadlocked ON ITSELF — while holding the lock. Every other thread
     *  that wants clientMtx then piles up behind it forever, the DSP thread included (onAudio
     *  takes it to read `audioClient`), so the whole server froze the instant a second listener
     *  arrived.
     *  ★★ IT COULD ONLY EVER FIRE IN MULTI-USER MODE. The condition short-circuits on
     *  `maxUsers <= 1`, so a single-user server never reaches the call — which is exactly why
     *  this survived: every path anyone had actually exercised skipped it. THIS, not the blocking
     *  broadcast, is what "multi-listener is BUILT BUT UNPROVEN" was really hiding.
     *  ★ Rule: a helper that takes a lock must never be called from a scope that holds it. Keep
     *  the two forms distinct and named so the next caller has to choose deliberately. */
    /** Is every slot taken, from the point of view of session `me`? Call WITH clientMtx held.
     *  ★ ONE predicate, shared by the accept path and the waiting loop. Two copies of this drift,
     *    and the drift is invisible: the queue would be telling people to come back for a slot the
     *    accept path does not agree is free. */
    bool isFullLocked(const std::string& me) {
        const int maxUsers = g_vsMaxUsers.load();
        return !occupantSession.empty()
            && occupantSession != me
            && ((specClient && specClient->isOpen()) || (audioClient && audioClient->isOpen()))
            && (maxUsers <= 1 || specListenerCountLocked() >= maxUsers);
    }

    /** Seconds until the earliest slot frees, or -1 when there is no honest answer.
     *  ★★ -1 IS A REAL ANSWER, NOT A FAILURE. With no session limit set there is no number we can
     *     stand behind — the occupant may stay all day. "In use, no fixed session limit" is
     *     honest; a countdown we cannot honour is worse than none, because the user WILL watch it.
     *     Call WITH clientMtx held. */
    int freeInSecsLocked() const {
        const int limitMin = g_vsSessionLimitMin.load();
        if (limitMin <= 0) return -1;
        if (occupantSession.empty() || occupantSince <= 0) return -1;
        if (occupantAddr.empty() || isLoopback(occupantAddr)) return -1;
        const double left = (double)limitMin * 60.0 - (Impl::nowSecs() - occupantSince);
        return left > 0 ? (int)(left + 0.5) : 0;
    }

    /** Hold a refused listener in the queue, telling them where they stand once a second, until
     *  a slot frees for them or they give up. Runs on the accepting socket's own thread.
     *  @return true if the caller should simply return (always — the socket is finished with). */
    void holdInQueue(const std::shared_ptr<net::Socket>& sock, const std::string& me) {
        const std::string addr = sock->peerAddress();
        {
            std::lock_guard<std::mutex> lk(clientMtx);
            // ★ Bounded. Each waiter costs a thread and a socket; past some depth the honest
            //   answer is "too many waiting" rather than a queue position nobody will reach.
            if ((int)waitQueue.size() >= kMaxWaiting) {
                sendWs(sock, 0x1, (const uint8_t*)"{\"type\":\"busy\",\"queueFull\":true}", 34);
                outboxClose(sock);
                return;
            }
            waitQueue.push_back({sock.get(), me, Impl::nowSecs(), sock->peerAddress()});
        }
        int lastPos = -1, lastFree = -2;
        while (serverRunning.load() && sock->isOpen()) {
            bool mine = false; int pos = 0, len = 0, freeIn = -1;
            {
                std::lock_guard<std::mutex> lk(clientMtx);
                len = (int)waitQueue.size();
                for (int i = 0; i < len; i++)
                    if (waitQueue[i].sock == sock.get()) { pos = i + 1; break; }
                freeIn = freeInSecsLocked();
                // ★★★ ONLY THE HEAD MAY CLAIM, and it claims by RESERVING ITS OWN ADDRESS. This is
                //     what makes the position we displayed true: for the next kReservationSec no
                //     other connection is admitted, so the person we told to come back is the
                //     person who gets in — not whoever's client happens to retry soonest.
                if (pos == 1 && !isFullLocked(me)
                    && (reservedUntil <= Impl::nowSecs() || reservedFor == me)) {
                    reservedFor  = me;
                    reservedUntil = Impl::nowSecs() + kReservationSec;
                    mine = true;
                }
            }
            if (mine) {
                // ★ Tell them to come back, then close. Reconnecting is what every client already
                //   does well; continuing this socket into a session would need the whole accept
                //   path to run again from the middle of a wait loop.
                char msg[96];
                int n = snprintf(msg, sizeof msg,
                                 "{\"type\":\"your_turn\",\"withinSec\":%d}", (int)kReservationSec);
                sendWs(sock, 0x1, (const uint8_t*)msg, (size_t)n);
                LOGI("queue: slot reserved for %s (%s) for %ds", me.c_str(), addr.c_str(),
                     (int)kReservationSec);
                break;
            }
            if (pos != lastPos || freeIn / 5 != lastFree / 5) {   // ★ only on a real change
                lastPos = pos; lastFree = freeIn;
                char msg[160];
                int n = snprintf(msg, sizeof msg,
                    "{\"type\":\"busy\",\"queuePos\":%d,\"queueLen\":%d,\"freeIn\":%d}",
                    pos, len, freeIn);
                // ★ sendWs returns void; the socket's own state is the liveness signal, and the
                //   loop condition already checks it every second.
                sendWs(sock, 0x1, (const uint8_t*)msg, (size_t)n);
                if (!sock->isOpen()) break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        {
            std::lock_guard<std::mutex> lk(clientMtx);
            for (size_t i = 0; i < waitQueue.size(); i++)
                if (waitQueue[i].sock == sock.get()) { waitQueue.erase(waitQueue.begin() + i); break; }
        }
        outboxClose(sock);
    }

    /** The sample rates THIS radio can actually be run at, newest-first, as a JSON array body.
     *  ★★★ ONE LIST, TWO CONSUMERS. The client's hwinfo has always carried this; the SETUP PAGE
     *      asked the owner to TYPE a rate instead, which is a question almost nobody can answer —
     *      "not everybody knows the spans their radio can handle" (Stuart, 2026-08-05) — and one
     *      where a wrong answer is rejected by the radio and reads as broken hardware. A second
     *      hand-written list on the page would drift from this one, which is the same failure in
     *      slower motion. So both read this.
     *  ★ Every entry here is a measured decision, not a datasheet figure — no 10 MSPS on the RSP,
     *    no 3.2 on the dongle: see the notes at the hwinfo site. */
    /** Admin challenge-response on an HTTP query string. ★ Extracted so a second endpoint cannot
     *  re-implement it slightly differently — the failure lockout and the "empty secret never
     *  authenticates" rule are both easy to leave out, and either omission is a hole. */
    bool adminOkFor(const std::string& reqLine, const std::shared_ptr<net::Socket>& sock) {
        std::string secret;
        { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
        const std::string ip = sock->peerAddress();
        const VsAdminProof pr = vsAdminProof(secret, reqLine);
        const bool ok = pr.ok && !g_vsAuthState.blocked(ip);
        if (ok) g_vsAuthState.recordOk(ip);
        else if (!secret.empty() && pr.guessable) g_vsAuthState.recordFail(ip);
        return ok;
    }

    std::string supportedRates() {
        // ★ Nothing to offer when this process holds no radio. Empty is what the setup page wants:
        //   it then falls back to the DRIVER table for the radio whose tab is open, which is the
        //   right answer per radio rather than one process's guess for all of them.
        if (frontDoorOnly) return "";
        if (useSdrplay()) return "8000000,6000000,5000000,4000000,3000000,2048000,2000000";
        if (useAirspyHf()) {
            // ★★★ THE HF+ GETS ONE RATE — ITS HIGHEST, WHICH IS ITS DEFAULT. This returned every
            //     rate the radio ADVERTISES, and most of them do not work here: the dead-lobe crop
            //     is a per-rate table whose numbers are measured at 912 kHz, and 228 kHz tunes
            //     about 7.8 kHz off frequency on this firmware (see nearestRate() in
            //     airspyhf_source.cpp, which has opened the radio at exactly one rate for months).
            //     So the radio was already running at one rate while this told every client it had
            //     seven — and the setup page, which offers what this returns, invited the owner to
            //     pick one of the broken ones.
            // ★★★ WHAT IT COST: choosing another rate left the crop computed for the wrong span,
            //     and Radio Caroline appeared at 665 kHz instead of 648. A receiver that is wrong
            //     about where it is listening is worse than one that offers no choice at all
            //     (Stuart, 2026-08-09 — and 2026-08-02: "I think we just offer the default on the
            //     server too otherwise that breaks all the dead space fix we added").
            // ★ Asked of the RADIO, not hard-coded: an HF+ Dual Port is not a Discovery, and the
            //   highest rate it reports is the right answer for whichever one is plugged in.
            if (auto* a = ahf.get()) {
                const auto& rl = a->sampleRates();
                if (!rl.empty()) return std::to_string(*std::max_element(rl.begin(), rl.end()));
            }
            return "";
        }
        return "2560000,2400000,1800000,1200000,960000";
    }

    int specListenerCountLocked() {
        int n = (specClient && specClient->isOpen()) ? 1 : 0;
        for (auto& s : specExtra) if (s && s->isOpen()) ++n;
        return n;
    }
    /** How many listeners are attached. Call WITHOUT clientMtx held. */
    int specListenerCount() {
        std::lock_guard<std::mutex> lk(clientMtx);
        return specListenerCountLocked();
    }
    std::shared_ptr<net::Socket> audioClient;
    // The single occupant's session id (empty = free). Guarded by clientMtx. A client's spectrum +
    // audio sockets share this id; a second client is refused while it is held. See acceptWs.
    std::string occupantSession;
    /** When the current occupant claimed the slot (seconds, monotonic). 0 = nobody. Guarded by
     *  clientMtx alongside occupantSession — one lock for one piece of state. */
    double occupantSince = 0;
    std::string occupantAgent;          ///< User-Agent of the single-user occupant, for the admin view
    unsigned long long soleLastBytes = 0;   ///< previous sample, for the uplink rate
    double soleLastAt = 0;
    double dspLoadPct = 0;              ///< last measured total DSP load, %
    /** The occupant's address, so a timeout can put THAT address on cooldown. */
    std::string occupantAddr;
    /** Warnings already sent this session, so each fires once: bit 0 = 2 min, bit 1 = 30 s.
     *  ★ A limit that ends a session with no warning reads as a crash. */
    int occupantWarned = 0;
    /** address -> monotonic time the cooldown ends. Pruned lazily on lookup. */
    std::map<std::string, double> cooldownUntil;

    // ── The waiting queue ──────────────────────────────────────────────────────────────────
    // ★★★ THE QUEUE *IS* THE SET OF WAITING SOCKETS, IN ARRIVAL ORDER. A refused listener used to
    //     be told "busy" and closed, which gives a person nothing to decide with — so they hammer
    //     reconnect, which our own cooldown then punishes. Holding the socket open instead means
    //     the queue needs no heartbeat, no expiry guess and no reaping: a waiter who gives up
    //     closes their socket and is gone, which is precisely the semantics we want and the only
    //     one that cannot drift out of step with reality.
    // ★★★ AND THE HEAD CLAIMS THE SLOT ITSELF. Showing someone "1st in the queue" and then handing
    //     the freed slot to whoever reconnects fastest is a lie the user can see. When the head
    //     notices a free slot it takes a short RESERVATION on its own address; every other
    //     connection is refused for its duration. A promised order the server does not keep is
    //     worse than telling them nothing (Stuart, 2026-08-04).
    struct Waiter { const net::Socket* sock; std::string who; double since; std::string ip; };
    std::vector<Waiter> waitQueue;              // arrival order; front = next served
    // ★★★ KEYED BY SESSION, NOT BY ADDRESS. The very same trap the occupancy check already
    //     documents: "browsers on the SAME machine share an IP but have different ids — which is
    //     why IP won't do". Reserving an ADDRESS means two people behind one NAT, or two tabs on
    //     one laptop, are the same claimant — so the second waiter walks into the slot the server
    //     just promised to the first, which is the exact failure the queue exists to prevent.
    //     Caught by queue.mjs, where both waiters are on 127.0.0.1.
    std::string reservedFor;                    // SESSION the next free slot belongs to
    double      reservedUntil = 0;              // ... until this time, then anyone may take it
    static constexpr int    kMaxWaiting      = 24;   // bounds the threads we hold open
    static constexpr double kReservationSec  = 25.0; // long enough to reconnect, short enough to
                                                     // not strand the slot if they walked away
    std::atomic<uint64_t> frameCounter{0};

    // ── ★★★ PER-CLIENT OUTBOX — see BUG-vibeserver-broadcast-blocks.md ──────────
    //
    // THE BUG THIS REPLACES. sendWs() used to take ONE GLOBAL `sendMtx` and then do a BLOCKING
    // write, and most of those calls happen on the DSP THREAD. So a single listener that stopped
    // reading — a slow link, a paused browser tab, a client mid-teardown — blocked in its own
    // send(), held the global lock, and froze EVERY other listener and the DSP thread with it.
    // Stuart, 2026-08-04: "I tried to connect to it whilst still being connected in the app and
    // both froze." That is the whole multi-user feature failing on its second user.
    //
    // ★★★ THE RULE NOW: NOTHING THAT PRODUCES DATA EVER WRITES TO A SOCKET.
    // Producers (DSP thread, decoder threads, control threads) only ever APPEND to a per-client
    // queue and return immediately. Each client has its OWN mutex and its OWN writer thread, so a
    // stalled peer backs up ITS OWN queue and nobody else can tell.
    //
    // ★★ `isOpen()` IS NOT A DEFENCE and never was — a socket can be perfectly open and simply not
    // draining. The only real defence is never blocking on it in the first place.
    // ★★★ Sig and RspStat are LIVE READOUTS, and a live readout is newest-wins exactly like a
    //     waterfall row: the moment a newer one exists the old one is not late, it is WRONG.
    //     They used to ride on Control, which is never dropped — harmless at 5 Hz, and the reason
    //     raising the signal meter to 20 Hz made it LAG WORSE rather than better: the frames
    //     queued, and every reading the client displayed was progressively further behind the
    //     radio (Stuart, 2026-08-05: "the whole signal meter is lagging now").
    //     ★ The lesson is that a rate rise is only safe on a channel that can DISCARD. Speeding up
    //       a never-drop stream converts spare bandwidth into latency.
    enum class Out { Control, Spectrum, Audio, Sig, RspStat };

    /** ★ Backlog ceiling per client. Reached = THIS listener cannot keep up, so THIS listener is
     *  dropped — which is the honest outcome and, crucially, a local one. The old code punished
     *  everybody for one slow peer; the whole point of this rewrite is that the cost stays with
     *  the client that incurred it. 512 KB is ~2.7 s of uncompressed audio, far more of Opus. */
    static constexpr size_t kOutboxMaxBytes = 512 * 1024;

    struct Outbox {
        std::shared_ptr<net::Socket> sock;
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::pair<Out, std::vector<uint8_t>>> q;
        size_t bytes = 0;
        /** ★ Cumulative bytes ever QUEUED for this socket. `bytes` above is the current backlog
         *  and goes up and down; this only rises, which is what a rate has to be computed from.
         *  Per listener it answers "what is this one costing my uplink" — and with the DSP time
         *  below, what a mode actually costs (Stuart, 2026-08-08: wanting a definitive answer on
         *  what FM stereo with RDS costs). */
        std::atomic<unsigned long long> sentTotal{0};
        bool   closing = false;      // drain what is queued, then the writer exits
        bool   overran = false;      // dropped for backlog, not for leaving
        std::thread th;
    };
    std::mutex outboxMtx;
    std::map<net::Socket*, std::shared_ptr<Outbox>> outboxes;

    /** The ONLY thread that ever writes to this client's socket. Blocking sends are fine HERE —
     *  blocking is exactly what this thread is for, and it holds no lock any other client wants. */
    void outboxWriter(std::shared_ptr<Outbox> ob) {
        vibeThreadName("vibeTx");
        for (;;) {
            std::pair<Out, std::vector<uint8_t>> msg;
            {
                std::unique_lock<std::mutex> lk(ob->m);
                ob->cv.wait(lk, [&]{ return ob->closing || !ob->q.empty(); });
                if (ob->q.empty()) return;                  // closing and drained
                msg = std::move(ob->q.front());
                ob->q.pop_front();
                ob->bytes -= msg.second.size();
            }
            if (!ob->sock->isOpen()) return;
            if (ob->sock->send(msg.second.data(), msg.second.size()) < 0) {
                ob->sock->close();                          // peer gone — its read loop unwinds
                return;
            }
        }
    }

    void outboxOpen(const std::shared_ptr<net::Socket>& sock) {
        auto ob = std::make_shared<Outbox>();
        ob->sock = sock;
        { std::lock_guard<std::mutex> lk(outboxMtx); outboxes[sock.get()] = ob; }
        ob->th = std::thread([this, ob]{ outboxWriter(ob); });
    }

    /** Stop accepting, let the writer drain what is already queued (bounded), then join.
     *  ★ THE DRAIN MATTERS: several paths say their piece and hang up in the next line —
     *  "cooldown", "needs_codec", the eviction notice. Closing without draining turns a server
     *  that EXPLAINS itself into one that hangs up silently, which reads to a user as a crash. */
    void outboxClose(const std::shared_ptr<net::Socket>& sock, int drainMs = 250) {
        std::shared_ptr<Outbox> ob;
        { std::lock_guard<std::mutex> lk(outboxMtx);
          auto it = outboxes.find(sock.get());
          if (it == outboxes.end()) return;
          ob = it->second; outboxes.erase(it); }
        { std::lock_guard<std::mutex> lk(ob->m); ob->closing = true; }
        ob->cv.notify_all();
        // Bounded wait: a peer that is not draining must not hold up the thread doing the teardown.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drainMs);
        for (;;) {
            { std::lock_guard<std::mutex> lk(ob->m); if (ob->q.empty()) break; }
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ob->sock->close();                    // unblocks the writer if it is stuck mid-send
        ob->cv.notify_all();
        if (ob->th.joinable()) ob->th.join();
    }

    /** Append a framed message for one client. NEVER blocks on the network. */
    void outboxPush(const std::shared_ptr<net::Socket>& sock, Out cls, std::vector<uint8_t>&& frame) {
        std::shared_ptr<Outbox> ob;
        { std::lock_guard<std::mutex> lk(outboxMtx);
          auto it = outboxes.find(sock.get());
          if (it != outboxes.end()) ob = it->second; }
        if (!ob) {
            // Not registered yet — the HTTP/handshake phase, where this thread is the only one
            // holding the socket and no other client can be affected. A direct write is safe and
            // keeps those paths behaving exactly as before.
            if (sock->isOpen()) sock->send(frame.data(), frame.size());
            return;
        }
        std::lock_guard<std::mutex> lk(ob->m);
        if (ob->closing) return;
        // ★★ SPECTRUM IS NEWEST-WINS. A waterfall row is worthless the moment a newer one exists,
        // so a listener who cannot keep up should fall BEHIND IN DETAIL, not accumulate a backlog
        // and then be dropped for it. Replacing rather than queueing is what lets a slow client
        // stay connected and merely run at a lower effective frame rate.
        // ★ Each newest-wins class supersedes only ITS OWN kind — a fresh signal reading must not
        //   discard the pending gain telemetry, which is a different quantity that has not changed.
        if (cls == Out::Spectrum || cls == Out::Sig || cls == Out::RspStat) {
            for (auto it = ob->q.begin(); it != ob->q.end(); ) {
                if (it->first == cls) { ob->bytes -= it->second.size(); it = ob->q.erase(it); }
                else ++it;
            }
        }
        // ★ Audio and control are NOT droppable — a gap in audio is audible and a dropped control
        // reply desynchronises the client. If those alone overflow, the link genuinely cannot carry
        // this listener, so drop the listener.
        if (ob->bytes + frame.size() > kOutboxMaxBytes) {
            if (!ob->overran) {
                ob->overran = true;
                LOGI("listener %s dropped — %zu KB backlog, cannot keep up",
                     ob->sock->peerAddress().c_str(), ob->bytes / 1024);
            }
            ob->closing = true;
            ob->cv.notify_all();
            ob->sock->close();
            return;
        }
        ob->bytes += frame.size();
        ob->sentTotal.fetch_add(frame.size(), std::memory_order_relaxed);
        ob->q.emplace_back(cls, std::move(frame));
        ob->cv.notify_one();
    }

    double specAuditMs = 0.0; long long specAuditFrames = 0;   // see onSpectrum's rate audit

    // ── Spectrum callback (Stage 3) ────────────────────────────────────────
    // The V5 engine hands us a fftshifted dB row (bin 0 = -fs/2, bins/2 = DC),
    // one per FFT, at FFT_AVG× the emit rate. We block-average FFT_AVG of them to
    // kill shimmer, then crop/zoom to OUT_BINS and key squelch/SNR — all in the
    // fftshifted layout (DC at bins/2), unlike the old raw-order IQFrontEnd path.
    static void specCb(void* ctx, const float* db, int bins) { ((Impl*)ctx)->onSpectrum(db, bins); }
    static void zoomSpecCb(void* ctx, const float* db, int bins) { ((Impl*)ctx)->onZoomSpectrum(db, bins); }

    // ★★★ HAND OVER TO THE ZOOM SPECTRUM once the view is narrower than the wide FFT can resolve.
    // `step` is source-bins-per-output-bin, so step < 1.0 IS "zoomed past real resolution" — below
    // that the wide path is magnifying nothing, which is what made high zoom blocky. Above it the
    // wide path is both correct and cheaper, so it keeps the job.
    // ★ Not on SpyServer: there the waterfall is the SERVER's FFT and we have no IQ for the view.
    bool zoomWasOn_ = false;
    long long zoomFrames_ = 0;
    void updateZoomView() {
        const double shown    = displaySpan() / zoomFactor.load();
        const double srcBinHz = sampleRate / (double)fftSize;
        const double step     = (shown / (double)wireBins()) / srcBinHz;
        // ★★★ TRACK THE WIRE WIDTH. The client picks its bin count when the SPECTRUM SOCKET
        //     CONNECTS, which is after startEngine() has already built the zoom FFT — so the two
        //     disagreed, every zoom frame was dropped by the width check below, and because the
        //     wide path is suppressed while zoom owns the waterfall the display simply FROZE
        //     (Stuart, 2026-08-02: "it worked and then froze as i zoomed in"). Re-applying it here
        //     is cheap: setZoomBins only rebuilds when the number actually changes.
        rx.setZoomBins(wireBins());
        // ★ A KILL SWITCH. The zoom path SUPPRESSES the wide one, so any fault in it takes the
        //   waterfall with it. Off = the server behaves exactly as it did before any of this.
        const bool want = !useSpy() && g_vsZoomSpectrum.load() && step < 1.0 && shown > 0.0;
        // ★★★ MINUS HW_OFFSET_HZ. The IQ the engine sees is baseband around the PHYSICAL DC,
        //     which offset tuning puts HW_OFFSET_HZ ABOVE rtlCenter — so a view offset measured
        //     from rtlCenter is wrong by exactly that much. The wide path has always subtracted
        //     it (`- hwOffsetBin` in onSpectrum); the zoom path did not, and the whole spectrum
        //     sat 15 kHz off (Stuart, 2026-08-02: "spectrum misaligned"). Same term, same
        //     mistake, second place today — see the tuneHw note in startEngine.
        // ★★ `fftRate` is the CLIENT-facing rate. rx's own fftRate_ is FFT_AVG times higher —
        //    the wide path averages that back down, the zoom path does not, so passing the engine
        //    rate here emitted FFT_AVG frames for every one asked for.
        if (want) rx.setZoomView(viewCenter.load() - rtlCenter.load() - hwOffsetHz(), shown, fftRate);
        else      rx.setZoomView(0.0, 0.0, fftRate);
        // Speaks on the TRANSITION only. The wide path is suppressed while the zoom path owns the
        // waterfall, so "zoom engaged" and "no frames" together means a BLANK display — which is
        // exactly the failure worth being able to see in a log rather than deduce.
        if (want != zoomWasOn_) {
            zoomWasOn_ = want;
            LOGI("zoom spectrum %s: view %.6f MHz, span %.3f kHz, step %.3f | "
                 "rtlCentre %.6f MHz, offset %.3f kHz, hwOff %.1f kHz",
                 want ? "ENGAGED" : "released",
                 viewCenter.load() / 1e6, shown / 1e3, step,
                 rtlCenter.load() / 1e6,
                 (viewCenter.load() - rtlCenter.load() - hwOffsetHz()) / 1e3,
                 hwOffsetHz() / 1e3);
            zoomFrames_ = 0;
        }
    }

    // A zoom frame, in the SAME wire format as onSpectrum's — same SPEC header, same bin count,
    // same dB offset — so the client needs no new code path and simply sees a sharper picture.
    void onZoomSpectrum(const float* db, int nb) {
        // ★ One radio, one view — every listener sees the SAME spectrum. What can differ is the
        //   WIDTH each asked to receive it at, so build one frame per distinct width.
        auto peers = allSpecPeers();
        if (peers.empty()) return;
        const int wire = wireBins();
        if (nb != wire) {
            // Must never happen now updateZoomView() tracks the wire width — but SAY SO if it
            // does. Dropping frames silently here is precisely what turned a width mismatch into
            // an unexplained frozen waterfall.
            if (zoomFrames_ >= 0) { zoomFrames_ = -1;
                LOGI("zoom spectrum DROPPED: %d bins but the wire wants %d", nb, wire); }
            return;
        }
        // ★★ Per-listener rate, exactly as the wide path does it — and the advance happens HERE
        //    because this path SUPPRESSES that one (see `rx.zoomSpanHz()` in onSpectrum). Exactly
        //    one of the two advances each accumulator per frame; doing it in both would give every
        //    listener twice the rate it asked for.
        auto due = dueForFrame(peers);
        {
            bool any = false;
            for (char d : due) if (d) { any = true; break; }
            if (!any) return;
        }
        std::vector<int> widths;
        for (size_t pi = 0; pi < peers.size(); pi++)
            if (due[pi] && std::find(widths.begin(), widths.end(), peers[pi].bins) == widths.end())
                widths.push_back(peers[pi].bins);
        for (int outBins : widths) {
        // ★★ The zoom row arrives at `nb` REAL bins (the widest listener's width). A listener who
        //    asked for fewer is peak-held DOWN from it — never interpolated up — so a watch gets a
        //    128-bin view of the same sharp data instead of forcing everyone to its width.
        const int grp = (outBins > 0 && nb > outBins) ? (nb / outBins) : 1;
        std::vector<uint8_t> frame(22 + outBins);
        frame[0]='S';frame[1]='P';frame[2]='E';frame[3]='C';frame[4]=0x01;frame[5]=0x03;
        uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::memcpy(&frame[6], &ts, 8);
        uint64_t f = (uint64_t)llround(viewCenter.load());
        std::memcpy(&frame[14], &f, 8);
        // ★★★ THE WIRE PUTS DC AT BIN 0 — IT IS NOT FFTSHIFTED. onSpectrum builds every frame as
        //     `signedOut = (i <= outBins/2) ? i : i - outBins`: bin 0 is the VIEW CENTRE, positive
        //     offsets ascend, negatives live in the top half. ZoomSpectrum hands back a SHIFTED
        //     row (DC in the middle, the layout a waterfall usually wants), so sending it straight
        //     out rotated every zoom frame by half a span against the format the client has always
        //     been given: a signal at the view centre landed at the frame EDGE, at every zoom
        //     level (Stuart, 2026-08-02: "the buzzer is on the left edge of the screen on both").
        //     Un-shift here rather than changing ZoomSpectrum, because shifted is the sane thing
        //     for a spectrum class to return and this is the one place that knows the wire format.
        const int half = outBins / 2;
        for (int i = 0; i < outBins; i++) {
            const int signedOut = (i <= half) ? i : i - outBins;   // same rule as onSpectrum
            int src = (half + signedOut) * grp;                    // -> index into the SHIFTED row
            if (src < 0) src = 0; else if (src >= nb) src = nb - 1;
            float best = db[src];
            for (int k = 1; k < grp && src + k < nb; k++)          // peak-hold, don't drop carriers
                if (db[src + k] > best) best = db[src + k];
            int v = (int)lround(best + 256.0);
            frame[22+i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        size_t sent = 0;
        for (size_t pi = 0; pi < peers.size(); pi++)
            if (due[pi] && peers[pi].bins == outBins)
                { sendWs(peers[pi].sock, 0x2, frame.data(), frame.size(), Out::Spectrum); sent++; }
        vsSpecBytes.fetch_add(frame.size() * sent, std::memory_order_relaxed);
        }   // end per-width loop
        if (++zoomFrames_ == 1 || zoomFrames_ % 100 == 0)
            LOGI("zoom spectrum: %lld frames sent", (long long)zoomFrames_);
    }
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
        // ★★ ONE FRAME PER DISTINCT WIDTH, not one frame for everybody. In practice that is a
        //    single frame (all browsers) or two (a browser and a watch) — the loop below groups
        //    peers by the width each asked for, so nobody's waterfall is resized by somebody
        //    else's arrival. See clientBins.
        auto peers = allSpecPeers();
        std::shared_ptr<net::Socket> sock = peers.empty() ? nullptr : peers.front().sock;

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
        // ★ While the zoom path is delivering, it owns the waterfall — emitting both would double
        //   the frame rate and make the two fight over the same texture rows.
        if (emit && rx.zoomSpanHz() > 0.0) emit = false;

        // ★★★ PER-LISTENER RATE, applied HERE and not in the engine. The engine runs at the
        //     fastest rate anyone asked for (recomputeEngineRate); this drops the frames a slower
        //     listener did not want, for that listener alone. Advanced only when this path is the
        //     one emitting — the zoom path suppresses it above and does its own advance.
        // ★ If nobody is due, skip the whole crop-and-downsample block: at 5 fps against a 20 fps
        //   engine that is three frames in four of the only expensive work left per listener.
        std::vector<char> due;
        if (emit) {
            due = dueForFrame(peers);
            bool any = false;
            for (char d : due) if (d) { any = true; break; }
            if (!any) emit = false;
        }

        if (emit && sock && sock->isOpen()) {
            // Emit a FIXED OUT_BINS bins (GPU-safe waterfall texture width — a
            // 32768-wide texture exceeds mobile GPU limits and the waterfall
            // silently fails). Map the fine internal FFT (bins) to the output,
            // applying zoom: each output bin covers `step` source bins; peak-hold
            // when downsampling (don't drop narrow carriers).
            double zoom = zoomFactor.load();
            // Distinct widths among the listeners, so the expensive part runs once per WIDTH
            // rather than once per listener.
            // ★★★ ONE FRAME PER DISTINCT VIEW, NOT PER WIDTH.
            //     Grouping only by width meant every listener got the frame cropped to the GLOBAL
            //     zoom — so a listener's own zoom did nothing until it was narrow enough to earn a
            //     private channel, and the waterfall jumped straight from "all 8 MHz" to "20 kHz"
            //     with nothing in between (Stuart, 2026-08-05: "zoom now has maximum out and
            //     maximum in"). The crop is per listener, so the GROUP is per listener's view.
            //     ★ Cheap: the FFT is shared and already paid for; only the crop and downsample
            //       repeat, and in practice everyone zoomed out shares one view.
            struct ViewKey { int bins; double centre, span; };
            std::vector<ViewKey> views;
            for (size_t pi = 0; pi < peers.size(); pi++) {
                auto& p = peers[pi];
                if (!due[pi]) continue;      // not this listener's frame — don't build its view
                auto c = dspFor(p.sock);
                // ★ `viewPriming` keeps a listener on the shared row for the moment its own view
                //   takes to fill — see ClientDsp::viewPriming. Without it the display freezes.
                if (c && c->ownView && !c->viewPriming.load()) continue;
                const double sp = (c && c->viewSpanHz > 0) ? c->viewSpanHz : shownHz;
                const double ce = (c && c->viewSpanHz > 0) ? c->viewCentreHz : viewCenter.load();
                bool seen = false;
                for (auto& v : views)
                    if (v.bins == p.bins && std::fabs(v.centre - ce) < 1 && std::fabs(v.span - sp) < 1)
                        { seen = true; break; }
                if (!seen) views.push_back({p.bins, ce, sp});
            }
            for (auto& view : views) {
            const int outBins = view.bins;
            // Source bins per output bin. Written in terms of the DISPLAY span so it
            // stays correct when that is decoupled from the IQ rate (SpyServer).
            // Reduces to bins/(zoom*outBins) whenever displaySpan == sampleRate.
            const double srcBinHz = sampleRate / (double)bins;
            const double step = (view.span / (double)outBins) / srcBinHz;  // src bins / out bin
            std::vector<uint8_t> frame(22 + outBins);
            frame[0]='S';frame[1]='P';frame[2]='E';frame[3]='C';frame[4]=0x01;frame[5]=0x03;
            uint64_t ts = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::memcpy(&frame[6], &ts, 8);
            // Offset tuning: the physical DC sits HW_OFFSET_HZ above rtlCenter, so
            // shift the crop down by that many source bins to keep the display
            // centred on the logical centre (rtlCenter) — the DC spike then draws
            // HW_OFFSET_HZ off-centre, harmlessly outside the channel.
            const double hwOffsetBin = hwOffsetHz() * (double)bins / sampleRate;
            // The display centre is viewCenter, which may sit off the dongle
            // centre (rtlCenter) — shift the crop by their difference so the user
            // can pan the view across the captured band while the dongle (and the
            // tuned VFO) stay put. dbAt clamps past the capture edge → floor.
            const double viewOffsetBin = (view.centre - rtlCenter.load()) * (double)bins / sampleRate;
            uint64_t f = (uint64_t)llround(view.centre);         // display centre = THIS view's centre
            std::memcpy(&frame[14], &f, 8);
            // ★★★ NOTHING IS RENDERED OUTSIDE THE CAPTURED BAND — u8 0 MEANS "NO DATA".
            //     The comment above used to claim dbAt "clamps past the capture edge → floor".
            //     It does not: it clamps the INDEX, so every position beyond the edge returns the
            //     EDGE BIN'S VALUE, over and over. Panning past the band therefore painted a wide
            //     smear of whatever happened to sit on the boundary — and, because it is real
            //     signal-level data as far as anything downstream can tell, it was dragged into
            //     the trace's AUTO-CONTRAST and squashed the part of the band that is actually
            //     there (Stuart, 2026-08-03: "it seems to think waterfall should be there when
            //     its not").
            //     ★ 0 is free as a sentinel: it means -256 dBFS, and the engine's floor is
            //       around -125. A real bin can never reach it.
            const int loValid = -bins / 2, hiValid = bins / 2;
            for (int i = 0; i < outBins; i++) {
                int signedOut = (i <= outBins / 2) ? i : i - outBins;
                double center = signedOut * step - hwOffsetBin + viewOffsetBin;  // signed src offset from DC
                int lo = (int)std::floor(center - step / 2.0);
                int hi = (int)std::ceil(center + step / 2.0);
                if (hi <= lo) hi = lo + 1;
                if (hi <= loValid || lo >= hiValid) { frame[22+i] = 0; continue; }  // wholly outside
                if (lo < loValid) lo = loValid;      // partly outside: peak-hold the REAL part only
                if (hi > hiValid) hi = hiValid;
                float best = -1e9f;
                for (int s = lo; s < hi; s++) {
                    float val = dbAt(s);                    // averaged dB
                    if (val > best) best = val;             // peak-hold
                }
                int v = (int)lround(best + 256.0);
                // Clamp to 1, not 0 — 0 is the no-data sentinel and must stay unambiguous.
                frame[22+i] = (uint8_t)(v < 1 ? 1 : (v > 255 ? 255 : v));
            }
            size_t sent = 0;
            for (size_t pi = 0; pi < peers.size(); pi++) {
                auto& p = peers[pi];
                if (!due[pi]) continue;      // this frame is not on this listener's clock
                if (p.bins != outBins) continue;
                auto c = dspFor(p.sock);
                // ★★ A listener drawing its OWN zoomed view must not also receive the shared wide
                //    row: two sources writing one waterfall doubles the frame rate and the two
                //    fight over the same texture, which is the exact failure the shared zoom path
                //    documents. Its own channel owns its display until it zooms back out.
                if (c && c->ownView && !c->viewPriming.load()) continue;
                const double sp = (c && c->viewSpanHz > 0) ? c->viewSpanHz : shownHz;
                const double ce = (c && c->viewSpanHz > 0) ? c->viewCentreHz : viewCenter.load();
                if (std::fabs(sp - view.span) >= 1 || std::fabs(ce - view.centre) >= 1) continue;
                sendWs(p.sock, 0x2, frame.data(), frame.size(), Out::Spectrum); sent++;
            }
            vsSpecBytes.fetch_add(frame.size() * sent, std::memory_order_relaxed);
            }   // end per-width loop
            // (byte accounting happens inside the per-width loop now)
            // (fm meta is serviced OUTSIDE this gate — see FM META SERVICE below)
            // ★ The RSP's AGC kick and its gain telemetry USED TO LIVE HERE, and that was the bug:
            //   this block is skipped whenever the zoom spectrum owns the waterfall (see `emit`
            //   above), so on a --zoom-spectrum server neither ever ran. They are now hoisted out
            //   of the emit gate — see "RSP GAIN SERVICE" below the block.
            // ★ The Advanced RDS payload runs FASTER than the metadata — a constellation
            // updated once a second reads as a still image, and its whole value is watching
            // the cloud tighten or spread as you tune. ~6 Hz, and only while the decoder is
            // open, so an ordinary listener never pays for it.
            // ★★★ A RATE, NOT A DIVISOR OF SOMEBODY ELSE'S RATE. This was `n % 2 == 0`, and the
            // comment above it has always said "~5 Hz" — true when the spectrum ran at 10 fps.
            // It is not a constant, it is a SHADOW of the spectrum rate, and that rate has since
            // moved twice: at the 14 fps we were really delivering it was 7 Hz, and this morning's
            // overlap fix took it to 11. Nobody touched the analyser, but it began re-rendering
            // 57% more often — a big panel with a constellation, an MPX spectrum and a symbol
            // trace — and the waterfall went jerky underneath it (Stuart, 2026-08-02: "I'm sure
            // it never used to do that", with the wire showing a healthy 31k/s · 22fps, so the
            // cost was in the RENDER, not the link).
            // ★★ It failed in the other direction too, and that was already known and written
            // down in SDRScreen's idle-saver: dropping the spectrum to 5 fps halved the analyser
            // to ~2.5 Hz, so the saver had to be suppressed entirely whenever the panel was open.
            // A quantity that is meant to be steady must not be derived from one that is not.
            if (rdsxOn.load()) {
                const double now = nowSecs();
                if (now - rdsxLastAt >= 1.0 / 6.0) { rdsxLastAt = now; sendRdsExt(sock); }
            }
            if (n % 2 == 0) { enforceSessionLimit(); enforceAdminIdle(); }
        }

        // ★★★ STATION PRESENCE / NOISE FLOOR — HOISTED OUT OF THE `emit` BLOCK (2026-08-03).
        //     It is labelled "full-span, zoom-independent" and it genuinely is, but it was
        //     computed INSIDE the emit gate, so it stopped updating entirely whenever the zoom
        //     spectrum owned the waterfall. That is the "SNR bar seems laggy" — it was not lagging,
        //     it was FROZEN at its last wide-path value (Stuart, 2026-08-03).
        //     ★ The tell was that the SQUELCH meter was fine: `channelDb` below has always been
        //       outside the gate. Two meters fed by the same FFT, one stale and one live, and the
        //       only difference between them was which side of this brace they sat on.
        {
            double binHz = sampleRate / (double)bins;
            int half = std::min(bins / 4, (int)(100000.0 / binHz));
            int skip = std::min(half - 1, std::max(1, (int)(3000.0 / binHz)));
            double cSum = 0; int cN = 0;
            for (int i = skip; i <= half; i++)           { cSum += dbAt(i); cSum += dbAt(-i); cN += 2; }
            // ★★★ MEASURE THE FLOOR WHERE THE RADIO CAN STILL HEAR. This sampled the OUTERMOST
            //     bins — and on an HF+ the outer 72 kHz per side is filter skirt, not band. The
            //     window is bins/8 per side, which at 912 kHz is 50 kHz: entirely INSIDE the dead
            //     lobe. So the "noise floor" was the anti-alias filter's stop-band, tens of dB
            //     below anything real, and the SNR came out correspondingly enormous — 61 dB on
            //     20 m against the dongle's 8 dB on the same signal at the same moment (Stuart,
            //     2026-08-09).
            // ★★ A DONGLE HID IT. At 2.4 MSPS the same window is 50 kHz of gentle roll-off rather
            //    than a brick wall, so the dongle read about 5 dB optimistic and looked fine. The
            //    fault was always there; only the HF+'s unusually wide skirts made it obvious.
            // ★ edgeCutoffHz() already knows where the usable band ends — the display crop uses it.
            //   Start the window just inside that, and clamp so a narrow capture cannot invert it.
            const int deadBins = std::min(bins / 4,
                                          (int)std::lround(edgeCutoffHz() / binHz));
            // ★★★ A MEDIAN, NOT A MEAN — BECAUSE ONE EDGE IS OFTEN A TRANSMITTER. This averaged
            //     both band edges, so a strong station sitting near the edge of the span WAS the
            //     noise floor: the figure rose to meet the signal and the meter read SNR 0 dB on a
            //     station plainly out of the noise (Stuart, 2026-08-15, 103.8 with a blowtorch at
            //     104.15 filling the top of the span).
            // ★★ It is the same mistake the client's own comment warns about at the other end —
            //    "not the mean: a strong carrier drags a mean upward and the SNR reads low exactly
            //    when the signal is strongest" — and the mean was kept here while being rejected
            //    there. A percentile does not care that a quarter of the window is occupied.
            // ★ 25th percentile of the edge bins, via nth_element: O(n), no sort, and the vector is
            //   reused so this allocates nothing per frame.
            edgeBins_.clear();
            for (int i = 0; i <= half / 2; i++) {
                const int lo = -(bins/2) + deadBins + i;
                const int hi = (bins/2 - 1) - deadBins - i;
                if (lo >= hi) break;
                edgeBins_.push_back((float)dbAt(lo));
                edgeBins_.push_back((float)dbAt(hi));
            }
            double floorDbNow = 0.0; const int eN = (int)edgeBins_.size();
            if (eN) {
                const size_t k = edgeBins_.size() / 4;
                std::nth_element(edgeBins_.begin(), edgeBins_.begin() + k, edgeBins_.end());
                floorDbNow = edgeBins_[k];
            }
            spectrumSnr.store((cN && eN) ? (float)(cSum/cN - floorDbNow) : 0.0f);
            // Band-edge average = our own noise floor, in the engine's dBFS. emitServerFft()
            // aligns the server's differently-scaled dB onto this, so the two waterfall sources
            // agree and the colour map is fed the values it was designed for.
            if (eN) iqFloorDb.store((float)floorDbNow);
        }

        // ══ FM META SERVICE ══════════════════════════════════════════════════════════════
        // ★★★ OUTSIDE THE EMIT GATE, for the same reason the RSP gain service below is. The call
        //     lived inside the wide-spectrum block, which is SKIPPED entirely whenever the zoom
        //     spectrum owns the waterfall — and --zoom-spectrum is on for every shared receiver we
        //     ship. So RDS was not merely per-client-broken, it was never sent at all on the
        //     configuration this product actually runs in. Third symptom of that one gate; the
        //     comment above it has been warning about exactly this since the AGC kick.
        // ★★ AND TO EVERY PEER. It went to one socket, so on a shared receiver only whoever
        //    happened to be the primary listener could ever see RDS.
        if ((n % 10) == 0) sendFmMetaAll();

        // ══ RSP GAIN SERVICE ═════════════════════════════════════════════════════════════
        // ★★★ DELIBERATELY OUTSIDE THE `emit` BLOCK ABOVE, AND THAT IS THE WHOLE POINT.
        //     It used to live inside it, which meant BOTH the AGC kick and the gain telemetry
        //     were suppressed whenever the zoom spectrum owned the waterfall (`emit = false`).
        //     On a --zoom-spectrum server — which is what the Pi demo runs — the AGC was
        //     therefore never kicked and no client ever heard the gain state at all: system
        //     gain read "—", the IF thumb never moved, and because the client only ever clears
        //     its read-only styling when a stat ARRIVES, the IF slider could never be unlocked
        //     even by an admin who had just turned the AGC off (Stuart, 2026-08-03).
        //     ★ Gain has nothing to do with which spectrum path is drawing. Never gate it on one.
        //
        // ★★ THREAD: this is onSpectrum, which runs on the dspThread (rx.feed). That has ALWAYS
        //    been true — an older comment here claimed this ran on a separate "client-emit path"
        //    and that moving it to the DSP thread caused the SEGV. It did not; the thread never
        //    changed. What actually crashed was running these sdrplay_api Update calls with no
        //    stream up. The property to preserve is therefore NOT a thread and NOT a listener,
        //    it is: THE STREAM MUST BE RUNNING. Being inside onSpectrum already proves that —
        //    it is only reached from rx.feed(), i.e. with real samples flowing — and `n > 10`
        //    below keeps it clear of the first frames after init, which is what actually crashed.
        //
        // ★★★ SO THE KICK NO LONGER NEEDS A LISTENER, AND MUST NOT. If someone connects, wakes
        //     the radio and disconnects two seconds later, the settle has to RUN TO COMPLETION
        //     rather than freezing half-done and leaving the next arrival on a stuck AGC
        //     (Stuart, 2026-08-03). When capture really does park, onSpectrum stops being called
        //     at all, so this needs no guard of its own for that case.
        if (useSdrplay() && sdrp) {
            // ★★★ THE INIT SEQUENCE, as performed by hand on the Mac where it works
            //     (Stuart, 2026-08-03). The API starts its loop on a TRANSITION, and a
            //     transition with no register write behind it can leave the loop inert —
            //     which is the "RSP auto gain gets stuck" this exists to cure.
            //       0. RF gain to its working point (LNA state), if it is not there already.
            //       1. AGC OFF, and stay off for ~a second.
            //       2. IF gain to MAXIMUM ATTENUATION / minimum gain (gRdB 59).
            //       3. Nudge a couple of dB toward more gain (59 -> 55) — a real register write.
            //       4. Back down to minimum gain (59), so the loop is handed a written register
            //          AND converges UPWARDS into place rather than starting overloaded.
            //       5. AGC ON.
            //     ★ SPACED, NOT BACK-TO-BACK: each step is an sdrplay_api Update and the next
            //       must not be issued until it has landed. On consecutive frames (~12 ms at
            //       8 MSPS) they outran the API and the wiggle did nothing. One step per ~20
            //       frames ≈ 1 s, which is also what "disables for a second" asks for.
            //     ★ AND IT SAYS WHAT IT DID — a silent fix cannot be told apart from one that
            //       never ran, which is precisely the position this was in.
            // ★★★ THE KICK IS FOR AN AGC-ENABLED RECEIVER ONLY. It exists to settle the tuner's
            //     own loop, and its last act is to switch the AGC ON — so on a receiver the owner
            //     has deliberately set to a FIXED gain it is worse than useless: it walks the LNA
            //     and IF reduction around for a second and then hands control to the very loop
            //     that was turned off (Stuart, 2026-08-05: "our kick only fires on AGC being left
            //     in the enabled state"). Apply the saved gain once instead, and be done.
            // ★★★ UNSET NOW MEANS MINIMUM, NOT "LET THE AGC SORT IT OUT". Everything the owner has
            //     not chosen starts at the safe end and stays MANUAL: least RF gain, most IF
            //     attenuation, AGC off (Stuart, 2026-08-06). The kick exists to settle the tuner's
            //     own loop, so with that loop switched off there is nothing to settle and it is
            //     skipped entirely.
            //     ★★ THIS CHANGED A REAL DEFAULT. The kick used to hand over at RF gain 7/9 — the
            //        working point of Stuart's own demo, chosen when the only receiver was his.
            //        On somebody else's aerial that is a guess, and the wrong guess damages a
            //        front end. A quiet receiver costs one click to fix; an overloaded one may
            //        cost the hardware.
            // ★★★ "NEVER TOUCHED" IS THE TEST, NOT "UNSET". The protective minimums are for a
            //     receiver whose owner has not chosen a gain yet — a FRESH install. An existing
            //     server whose owner set an RF gain but left the IF on AGC has chosen; forcing it
            //     to minimum would silently deafen a working receiver on an upgrade, which is
            //     exactly what Stuart asked not to happen to the demo ("these gain settings are
            //     for going forward on new installs", 2026-08-06).
            //     ★ An upgrade must never change how a running receiver hears. That is worth more
            //       than the tidiness of one rule covering both cases.
            const int savedLna0 = g_vsSavedLna.load(), savedGr0 = g_vsSavedIfGr.load();
            const int savedAgc0 = g_vsSavedIfAgc.load();
            const bool neverTouched = (savedLna0 < 0 && savedGr0 < 0 && savedAgc0 < 0);
            if ((savedAgc0 == 0 || neverTouched) && sdrpAgcKick < 6 && n > 10) {
                const int savedGr  = g_vsSavedIfGr.load();
                // LNA state counts the OTHER WAY: 0 is maximum RF gain, so the last state is the
                // least. Derived from the ladder so an RSP1 (4 states) and a dx (28) both land at
                // their own minimum rather than at a literal that suits one model.
                const int minRfLna = std::max(0, sdrp->lnaStateCount() - 1);
                const int savedLna = g_vsSavedLna.load() >= 0 ? g_vsSavedLna.load() : minRfLna;
                sdrp->setIfAgc(false);
                sdrp->setLnaState(savedLna);
                sdrp->setIfGainReduction(savedGr >= 0 ? savedGr : 59);   // 59 = most attenuation
                sdrpAgcKick = 6;                       // nothing left to settle
                LOGI("RSP: manual gain — AGC off, lna %d, ifgr %d, sysGain %.1f dB "
                     "(no AGC kick: there is no loop to settle)",
                     sdrp->currentLnaState(), sdrp->currentIfGr(), sdrp->systemGainDb());
            }
            else if (sdrpAgcWanted && sdrpAgcKick < 6 && n > 10 && (n % 20) == 0) {
                switch (++sdrpAgcKick) {
                    case 1: sdrp->setLnaState(std::max(0, sdrp->lnaStateCount() - 1 - kRspInitRfGainPos));
                            LOGI("AGC kick 1/6: LNA state -> %d (RF gain %d/%d)",
                                 sdrp->currentLnaState(),
                                 sdrp->lnaStateCount() - 1 - sdrp->currentLnaState(),
                                 sdrp->lnaStateCount() - 1); break;
                    case 2: sdrp->setIfAgc(false);
                            LOGI("AGC kick 2/6: AGC off (ifgr now %d)", sdrp->currentIfGr()); break;
                    case 3: sdrp->setIfGainReduction(59);
                            LOGI("AGC kick 3/6: ifgr -> %d (max attenuation)", sdrp->currentIfGr()); break;
                    case 4: sdrp->setIfGainReduction(55);
                            LOGI("AGC kick 4/6: ifgr -> %d (up a couple of dB)", sdrp->currentIfGr()); break;
                    case 5: sdrp->setIfGainReduction(59);
                            LOGI("AGC kick 5/6: ifgr -> %d (back down, ready to hand over)",
                                 sdrp->currentIfGr()); break;
                    default: {
                            // ★★★ THE OWNER'S SAVED FRONT END WINS, AND IT MUST BE APPLIED HERE —
                            //     after the kick, not before. The kick deliberately walks the LNA
                            //     and IF reduction to settle the tuner, so anything applied earlier
                            //     is overwritten by the very next step and the owner's setting
                            //     vanishes with no trace of why.
                            const int savedLna = g_vsSavedLna.load(), savedGr = g_vsSavedIfGr.load();
                            const int savedAgc = g_vsSavedIfAgc.load();
                            if (savedLna >= 0) sdrp->setLnaState(savedLna);
                            // ★ AGC BEFORE the manual IF reduction: it owns the gain path, so
                            //   setting the reduction first and then enabling AGC would let the
                            //   loop immediately undo it. Same ordering rule as ahf_control.
                            sdrp->setIfAgc(savedAgc != 0);          // -1 (unset) => on, as before
                            if (savedAgc == 0 && savedGr >= 0) sdrp->setIfGainReduction(savedGr);
                            LOGI("AGC kick 6/6: %s (ifgr %d, lna %d, sysGain %.1f dB)%s",
                                 savedAgc == 0 ? "AGC off — owner's saved gain restored" : "AGC on",
                                 sdrp->currentIfGr(), sdrp->currentLnaState(), sdrp->systemGainDb(),
                                 (savedLna >= 0 || savedGr >= 0) ? " [saved]" : "");
                            break; }
                }
            }
            // Settling window: the whole kick plus a margin, reported to the client so the
            // waterfall's bounce is EXPLAINED rather than looking like a fault. An unexplained
            // transient reads as a defect; a labelled one reads as a radio settling.
            sdrpSettling = (sdrpAgcKick < 6);

            // ★ The RSP's live gain state. The AGC moves the IF reduction on its own, so a
            //   slider position is NOT the truth — and total system gain is the one figure that
            //   makes two independent controls readable.
            // ★ ~10 Hz, not 1. The IF slider follows the AGC live, and a thumb that jumps once a
            //   second reads as broken rather than as tracking. ~90 bytes is nothing next to the
            //   spectrum.
            // ★★ CUT TO THE PRIMARY SOCKET 2026-08-04 (it multiplied traffic through a BLOCKING
            //    write under a GLOBAL mutex on the DSP thread), then RESTORED TO EVERY PEER once
            //    that path was replaced by the per-client outbox — which is exactly the condition
            //    the cutback named. Queueing ~90 bytes per peer cannot block anyone.
            // ★★★ AND IT HAD TO COME BACK. This is the gain readout; the `sig` block below is the
            //    signal meter. On the primary socket only, EVERY listener after the first had a
            //    dead S-meter and a frozen gain display — and a control that is visible and inert
            //    reads as "the feature is broken", not "you are the second listener". A shared
            //    receiver whose meters only work for whoever connected first is not shared.
            if (n % 2 == 0) {
                char gb[160];
                snprintf(gb, sizeof gb,
                    "{\"type\":\"rspstat\",\"sysGain\":%.1f,\"lna\":%d,\"ifgr\":%d,\"overload\":%d,"
                    "\"settling\":%d}",
                    sdrp->systemGainDb(), sdrp->currentLnaState(), sdrp->currentIfGr(),
                    sdrp->overloaded() ? 1 : 0, sdrpSettling ? 1 : 0);
                for (auto& p : peers) sendText(p.sock, gb, Out::RspStat);
            }
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

            // ══ THE SIGNAL METER'S NUMBERS, MEASURED HERE AND SENT ═══════════════════════
            // ★★★ THE CLIENT USED TO DERIVE THESE FROM THE SPECTRUM FRAME IT HAD BEEN SENT, AND
            //     THAT IS WHY THE BAR TRACKED THE ZOOM. A frame's bins are whatever the current
            //     view is: zoom in and each bin is narrower, so a carrier's power lands in fewer
            //     of them while the per-bin noise floor drops — the meter climbed towards full
            //     scale purely because the user had zoomed (Stuart, 2026-08-03: "zoomed out the
            //     bar is lower, when zoomed in the bar is almost maxed out... the bar is relative
            //     to the zoom not the S meter").
            //     ★ These two are computed from the FULL-RATE FFT at a FIXED resolution, so they
            //       do not move when the view does. `peak` is the same quantity the SQUELCH uses
            //       — the one meter Stuart confirmed reads correctly — so the S meter now agrees
            //       with the squelch by construction instead of by coincidence.
            //     ★ OUTSIDE the emit gate, deliberately: see the RSP GAIN SERVICE note above.
            //       Putting a meter behind that gate is what froze the SNR bar in the first place.
            // ★★★ CUT BACK 2026-08-04 AFTER IT FROZE THE DEMO. This sent EVERY frame to EVERY
            //     peer, and both halves were a mistake — not because of bandwidth (~90 bytes), but
            //     because of WHERE the write happens. sendWs takes ONE GLOBAL sendMtx and then does
            //     a BLOCKING socket write, on the DSP THREAD. So a single stalled listener blocks
            //     every send to every other listener and stops the DSP. Stuart connected a second
            //     client while the app was connected and BOTH FROZE.
            //     ★ The hazard is older than this line (the spectrum broadcast above has the same
            //       shape), but multiplying the traffic through it by the peer count, at 20 Hz, is
            //       what made it fire. Back to the primary socket at ~5 Hz until the socket path
            //       itself is made non-blocking — see BUG-vibeserver-broadcast-blocks.md.
            //     ★ The meter's own smoothing copes with 5 Hz; it is the SCALE that mattered
            //       (a fixed dBFS scale, not the waterfall's auto-contrast), not the update rate.
            // ★★ RESTORED TO EVERY PEER 2026-08-04, for the reason the cutback itself gave: the
            //    write path can no longer block (per-client outbox). ~5 Hz is KEPT — that rate was
            //    fine on its own merits, since the meter's own smoothing copes and it was the
            //    SCALE that mattered, not the rate. Only the fan-out was the bug.
            // ★★★ EVERY FRAME, NOT EVERY FOURTH. The note above concluded that "the meter's own
            //     smoothing copes with 5 Hz; it is the SCALE that mattered, not the update rate" —
            //     which was true of the bug it was written for (the meter borrowing the
            //     waterfall's auto-contrast) and does not survive the next complaint: no amount of
            //     client-side smoothing can make a meter respond faster than its measurement
            //     ARRIVES, and at 5 Hz a signal is up to 200 ms old before the bar has been told
            //     (Stuart, 2026-08-05: "making the SNR bar more responsive").
            //     ★ The cost is the reason it was ever throttled, so: ~60 bytes at 20 Hz is
            //       1.2 KB/s per listener, about 36 KB/s with thirty of them — a third of a
            //       percent of the spectrum traffic they are already taking. It was the FAN-OUT
            //       through a blocking write that was expensive, and that is gone.
            {
                // ★★★ MEASURED PER LISTENER, AT THE FREQUENCY THAT LISTENER IS ACTUALLY ON.
                //     `peak` above is the SHARED VFO's passband — and in per-client mode the
                //     shared VFO is nobody's: a per-client `tune` goes to that listener's own
                //     ClientDsp and never touches `audioFreq`. So every listener's signal meter
                //     has been reporting a frequency no human selected since per-client DSP
                //     landed. It does not merely lag: it reads the WRONG BAND, and with the AGC
                //     riding the whole 8 MHz it moves INVERSELY to the signal you are listening
                //     to — a strong station keying up makes the AGC pull everything else down, so
                //     the bar FALLS as the signal arrives (Stuart, 2026-08-05, on the Buzzer:
                //     "the bar is going down when the buzz actually happens, the opposite of how
                //     it should react"). That is what pointed at the frequency rather than the
                //     rate; no amount of draw-rate work would have touched it.
                // ★★★ THE FAMILY: when a shared resource becomes per-listener, EVERY path that
                //     identified the listener has to be re-derived — not just the one you were
                //     looking at. This is the third in one session (the app's tune routing, the
                //     decoder feed, and now the meter). See [[clients_differ_which_socket]].
                const double binHz = sampleRate / (double)bins;
                const float floorDb = iqFloorDb.load();
                for (auto& p : peers) {
                    float mine = peak;                       // shared VFO — the fallback
                    if (auto c = dspFor(p.sock)) {
                        const double off = c->vfoHz - rtlCenter.load() - hwOffsetHz();
                        const int cb = (int)llround(off / binHz);
                        const int hw2 = std::max(1, (int)(c->bwHz / 2.0 / binHz));
                        float pk = -1e9f;
                        for (int o = -hw2; o <= hw2; o++) {
                            const float v = dbAt(cb + o);
                            if (v > pk) pk = v;
                        }
                        mine = pk;
                    }
                    char sb[128];
                    snprintf(sb, sizeof sb, "{\"type\":\"sig\",\"chan\":%.1f,\"floor\":%.1f}",
                             mine, floorDb);
                    sendText(p.sock, sb, Out::Sig);
                }
            }
        }
        // ★ The spectrogram is fed from the SHARED wide row — the one picture that is the same
        //   for everybody and cannot be moved by a listener. Feeding it from anyone's per-client
        //   view would make a 24-hour image out of wherever individual people happened to look.
        spectroFeed(fftAccum.data(), (int)fftAccum.size(), accumCount);
        measureBands(fftAccum.data(), (int)fftAccum.size(), accumCount);
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

        const uint32_t iqHz  = (uint32_t)llround(rtlCenter.load() + hwOffsetHz());
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
        // ★★★ COPY THE CLIENT OUT, THEN DROP THE LOCK BEFORE sendConfig — a SELF-DEADLOCK.
        //     sendConfig calls binsFor(), which locks clientMtx itself, and clientMtx is a plain
        //     std::mutex: re-locking it on the same thread is undefined and here it simply hung
        //     forever. The whole server froze — DSP heartbeat stopped, HTTP stopped answering —
        //     while the PROCESS stayed alive, so it read as a crash with no crash report.
        // ★★ Fired on any client sample-rate change, which is a normal thing to do from the
        //    audio menu. The two call sites above already copy-then-release; these two did not.
        std::shared_ptr<net::Socket> scfg;
        { std::lock_guard<std::mutex> lk(clientMtx); scfg = specClient; }
        if (scfg) sendConfig(scfg);
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

        const int outBins = binsFor(sock);
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
    static void rdsPsCb_(RdsState& st, double vfoHz, Impl* im, uint16_t pi, const char* ps8) {
        {
            std::lock_guard<std::mutex> lk(st.rdsMtx);
            st.rdsPi = pi; st.rdsPsName = ps8 ? ps8 : "";
        }
        // Learn the station against the frequency it was heard on. audioFreq is the
        // VFO — the thing actually being listened to — not the dongle centre.
        // ★ Learned against THIS listener's VFO — the frequency actually being heard. It used to
        //   read the shared audioFreq, which in per-client mode is nobody's, so a learned
        //   bookmark would be filed under a frequency no one was on.
        if (ps8) bmLearn(vfoHz, (int)pi, ps8);
    }
    static void rdsTextCb_(RdsState& st, double vfoHz, Impl* im, const char* rt64) {
        std::lock_guard<std::mutex> lk(st.rdsMtx);
        st.rdsText = rt64 ? rt64 : "";
    }
    // ★ PI on its own, the moment the decoder confirms it. It used to be set ONLY
    // inside rdsPsCb, so the station's identity was hostage to its NAME assembling —
    // and the name is the fragile part (8 characters across 4 groups, any one of which
    // can be lost), while PI is 16 error-protected bits repeated ~11 times a second.
    // Weak stations therefore reported nothing at all when they were in fact telling
    // us exactly who they were (2026-07-26).
    static void rdsPiCb_(RdsState& st, double vfoHz, Impl* im, uint16_t pi) {
        std::lock_guard<std::mutex> lk(st.rdsMtx);
        st.rdsPi = (int)pi;
    }
    static void rdsExtCb_(RdsState& st, double vfoHz, Impl* im, const vibedsp::RxPipeline::Callbacks::RdsExt& x) {
        if (!im || !im->rdsxOn.load()) return;
        std::lock_guard<std::mutex> lk(st.rdsMtx);
        st.rdsPty = x.pty; st.rdsTp = x.tp; st.rdsTa = x.ta; st.rdsMs = x.ms; st.rdsDi = x.di;
        st.rdsPtyRaw = x.ptyRaw; st.rdsTpRaw = x.tpRaw; st.rdsTaRaw = x.taRaw;
        st.rdsMsRaw = x.msRaw;   st.rdsDiRaw = x.diRaw;
        st.rdsCtMin = x.ctMinutes; st.rdsCtOff = x.ctOffsetHalfHours;
        st.rdsGrpTotal = x.groupTotal; st.rdsAfSeen = x.afSeen;
        st.rdsAf.assign(x.afKhz, x.afKhz + x.nAf);
        st.rdsAfAll.assign(x.afAllKhz, x.afAllKhz + x.nAfAll);
        st.rdsAfAllOk.assign(x.afAllOk, x.afAllOk + x.nAfAll);
        st.rdsGrp.assign(x.groupCounts, x.groupCounts + 32);
        st.rdsConst.assign(x.constXY, x.constXY + x.nPts * 2);
        if (x.mpx && x.nMpx > 0) st.rdsMpx.assign(x.mpx, x.mpx + x.nMpx);
        st.rdsRtpTitle = x.rtpTitle ? x.rtpTitle : "";
        st.rdsRtpArtist = x.rtpArtist ? x.rtpArtist : "";
        st.rdsLongPs = x.longPs ? x.longPs : "";
        st.rdsPtyn = x.ptyn ? x.ptyn : "";
        st.rdsLang = x.language;
        st.rdsPinDay = x.pinDay; st.rdsPinHour = x.pinHour; st.rdsPinMin = x.pinMinute;
        st.rdsEon.assign(x.eon, x.eon + x.nEon);
        st.rdsOda.assign(x.oda, x.oda + x.nOda);
        st.rdsPhase = x.pilotPhaseDeg;
        st.rdsPhaseCoh = x.pilotPhaseCoherence;
        st.rdsPhaseDrift = x.pilotPhaseDriftDegPerSec;
        st.rdsPilotDev = x.pilotDevKHz;
        st.rdsDev      = x.rdsDevKHz;
    }
    static void rdsSigCb_(RdsState& st, double vfoHz, Impl* im, float relDb) {
        std::lock_guard<std::mutex> lk(st.rdsMtx);
        st.rdsSig = relDb;
    }
    static void rdsBerCb_(RdsState& st, double vfoHz, Impl* im, int percent) {
        std::lock_guard<std::mutex> lk(st.rdsMtx);
        st.rdsBer = percent;
    }
    static void rdsEccCb_(RdsState& st, double vfoHz, Impl* im, uint8_t ecc) {
        std::lock_guard<std::mutex> lk(st.rdsMtx);
        st.rdsEcc = ecc;
    }
    static void stereoCb_(RdsState& st, double, Impl*, bool locked) { st.stereoDetected.store(locked); }

    // ── Callback plumbing ────────────────────────────────────────────────────────────────────
    // ★★ TWO THIN WRAPPERS OVER ONE BODY. RxPipeline::Callbacks carries a single `ctx`, shared
    //    with the audio callback — so the shared pipeline must pass Impl* and a per-listener one
    //    must pass ClientDsp*. Resolving which RdsState to write in the wrapper, and keeping the
    //    logic in one place, is what stops the two drifting: a fix applied to one and not the
    //    other would be indistinguishable from the bug it fixed.
    static void rdsPsCb(void* ctx, uint16_t pi, const char* ps8) {
        auto* t = (Impl*)ctx; rdsPsCb_(t->rdsS, t->audioFreq.load(), t, pi, ps8); }
    static void rdsTextCb(void* ctx, const char* rt64) {
        auto* t = (Impl*)ctx; rdsTextCb_(t->rdsS, 0, t, rt64); }
    static void rdsPiCb(void* ctx, uint16_t pi) {
        auto* t = (Impl*)ctx; rdsPiCb_(t->rdsS, 0, t, pi); }
    static void rdsExtCb(void* ctx, const vibedsp::RxPipeline::Callbacks::RdsExt& x) {
        auto* t = (Impl*)ctx; rdsExtCb_(t->rdsS, 0, t, x); }
    static void rdsSigCb(void* ctx, float relDb) {
        auto* t = (Impl*)ctx; rdsSigCb_(t->rdsS, 0, t, relDb); }
    static void rdsBerCb(void* ctx, int percent) {
        auto* t = (Impl*)ctx; rdsBerCb_(t->rdsS, 0, t, percent); }
    static void rdsEccCb(void* ctx, uint8_t ecc) {
        auto* t = (Impl*)ctx; rdsEccCb_(t->rdsS, 0, t, ecc); }
    static void stereoCb(void* ctx, bool locked) {
        auto* t = (Impl*)ctx; stereoCb_(t->rdsS, 0, t, locked); }


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
    /** Per-client twin of sendAudioPcm. Identical wire format — the difference is WHOSE encoder
     *  and WHOSE codec preference it uses. ★ An Opus encoder carries stream state, so it cannot be
     *  shared between listeners: two clients through one encoder interleave their frames and both
     *  get garbage. */
    void sendClientAudio(ClientDsp* c, const std::shared_ptr<net::Socket>& sock,
                         const int16_t* pcm, int count, int ch) {
        if (count <= 0) return;
        std::vector<int16_t> monoBuf;
        if (c->forceMono && ch == 2) {
            monoBuf.resize((size_t)count);
            for (int i = 0; i < count; i++)
                monoBuf[i] = (int16_t)(((int)pcm[i*2] + (int)pcm[i*2+1]) / 2);
            pcm = monoBuf.data(); ch = 1;
        }
#ifdef VIBE_HAVE_OPUS
        if (c->wantsOpus) {
            c->opus.setBitrate(g_vsOpusBitrate.load());
            std::vector<std::vector<uint8_t>> packets;
            c->opus.encode(pcm, count, ch, packets);
            const uint32_t sr = (uint32_t)vibe::OpusAudioEncoder::kSampleRate;
            for (auto& pkt : packets) {
                std::vector<uint8_t> frame; frame.reserve(6 + pkt.size());
                frame.push_back((uint8_t)ch); frame.push_back(3);
                frame.push_back((uint8_t)(sr & 0xff));         frame.push_back((uint8_t)((sr >> 8) & 0xff));
                frame.push_back((uint8_t)((sr >> 16) & 0xff)); frame.push_back((uint8_t)((sr >> 24) & 0xff));
                frame.insert(frame.end(), pkt.begin(), pkt.end());
                sendWs(sock, 0x2, frame.data(), frame.size(), Out::Audio);
                vsAudioBytes.fetch_add(frame.size(), std::memory_order_relaxed);
            }
            return;
        }
#endif
        std::vector<uint8_t> frame(6 + (size_t)count * ch * 2);
        frame[0] = (uint8_t)ch; frame[1] = 0;
        uint32_t sr = (uint32_t)AUDIO_SR; std::memcpy(&frame[2], &sr, 4);
        std::memcpy(frame.data() + 6, pcm, (size_t)count * ch * 2);
        sendWs(sock, 0x2, frame.data(), frame.size(), Out::Audio);
        vsAudioBytes.fetch_add(frame.size(), std::memory_order_relaxed);
    }

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
                sendWs(sock, 0x2, frame.data(), frame.size(), Out::Audio);
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
        sendWs(sock, 0x2, frame.data(), frame.size(), Out::Audio);
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
        // ★★★ NOT IN SHARED MODE. Here the shared pipeline's VFO is nobody's: every listener has
        //     their own. Decoding from it means decoding a frequency no human selected, so the
        //     decoders are driven from the OWNING listener's audio instead (onClientAudio).
        if (!perClientDsp()) {
            feedDecoder(data, count);
            feedSpots(data, count);
            decoderFedSamples.fetch_add((uint64_t)count, std::memory_order_relaxed);
        }

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
        if (!decoder && !wefax && !sstv && !timeDec) return;
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
        // ★★★ FALL THROUGH TO THE TEXT FLUSH — do NOT return here. The time decoder emits its
        //     minutes down the same text channel RTTY uses, and that channel is drained by the
        //     code BELOW this point. An early return processed the audio perfectly and then threw
        //     the result away: a decoder that runs, decodes, and is never heard from. Exactly the
        //     shape of the WEFAX bug documented in startDecoder — complete, correct code with
        //     nothing carrying its output.
        if (timeDec) timeDec->process(mono.data(), count);
        else if (decoder) decoder->process(mono.data(), count);
        else return;
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
    /** ★ Push the owner's notice to everyone listening RIGHT NOW. Sent to every spectrum client,
     *  not just the first: on a shared receiver the people who most need it are the ones already
     *  watching the spectrum misbehave. */
    void sendNoticeNow() {
        const std::string body = "{\"type\":\"notice\",\"text\":\""
                               + vibeadmin::esc(g_vsNotice.current()) + "\"}";
        for (auto& c : allSpecClients()) if (c && c->isOpen()) sendText(c, body);
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
        std::lock_guard<std::mutex> lk(rdsS.rdsMtx);
        rdsS.rdsPsName.clear(); rdsS.rdsText.clear(); rdsS.rdsPi = -1; rdsS.rdsEcc = 0; rdsS.rdsBer = -1; rdsS.rdsSig = -99.0f;
        rdsS.stereoDetected.store(false);
        // ★★★ AND ASK FOR IT BACK. The stereo report is EDGE-triggered and a retune inside the
        //     same mode does not rebuild the chain, so the pilot never unlocks and there is no
        //     edge to refill what we just cleared — the listener sat in mono on a plainly stereo
        //     station until something forced a rebuild (Stuart, 2026-08-08). Measured on the Pi:
        //     the DSP was locked with blend=1.00 the whole time; only the REPORT was missing.
        rx.requestStereoReport();
        // ★ And drop what the decoder learned from the PREVIOUS station — its timing hypothesis
        //   scores outlive a retune, and a stale one beats the correct one on a weak signal.
        rx.requestRdsResync();
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
        cb.zoomSpectrum = &Impl::zoomSpecCb;
        // The zoom FFT emits straight to the wire, so its width must BE the wire width.
        rx.setZoomBins(wireBins());
        // ★★★ Method is fixed for the life of the engine — never switched live (Stuart,
        // 2026-08-02). Shared (fast convolution) only makes sense with a LOCKED centre, since
        // every channel is a slice of one FFT of one captured band.
        const bool useShared = g_vsSharedChannels.load() && g_vsLockedCentre.load() > 0.0;
        rx.setSharedChannels(useShared);
        LOGI("channel method: %s (asked %s, centre %s)",
             useShared ? "SHARED / ka9q fast convolution" : "DIRECT / per-client DDC",
             g_vsSharedChannels.load() ? "shared" : "direct",
             g_vsLockedCentre.load() > 0.0 ? "locked" : "free");
        // ★ Make the zoom DSP say what it actually derived. Everything it does hangs off the
        //   PIPELINE's sample rate, not the shim's, and nothing upstream could show a disagreement.
        rx.setZoomLog([](double fs, double offHz, double reqSpan, double rawSpan,
                         int decim, int centreBin, int chanBins) {
            LOGI("zoom DSP: fs %.3f MHz, offset %.3f kHz, span %.3f kHz (raw %.3f), "
                 "decim %d, centreBin %d, chanBins %d",
                 fs / 1e6, offHz / 1e3, reqSpan / 1e3, rawSpan / 1e3, decim, centreBin, chanBins);
        });
        // A restart rebuilds the engine underneath the zoom channel, so its view has to be
        // re-applied — otherwise it keeps filtering around the centre the OLD rate implied.
        updateZoomView();
        // ★★★ OFFSET TUNING STILL HAS TO BE APPLIED WHEN THE CENTRE IS LOCKED. Every source
        //     opens at EXACTLY the centre it is given, and tuneHw() is the ONLY thing that adds
        //     HW_OFFSET_HZ — while vfoOffsetNow() subtracts it unconditionally. Unlocked, the
        //     first retune calls tuneHw() and the two agree; LOCKED, retune() returns early and
        //     never does, so the compensation was applied to a radio that had never been offset
        //     and every demod landed 15 kHz low (Stuart, 2026-08-02: tuned to the Buzzer on 4625,
        //     hearing Northwood fax on 4609). Apply it once here, where the source is already open.
        if (g_vsLockedCentre.load() > 0.0 && !useSpy()) {
            rtlCenter.store(g_vsLockedCentre.load());
            tuneHw(g_vsLockedCentre.load());
        }
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
    /** ★ Each listener has their own change-detection now (it lives in their RdsState), so this
     *  is a plain loop: nobody can suppress anybody else's message, and a listener who joins
     *  mid-song is told the station's name on their first tick because their own lastSent* are
     *  empty. The peer-tracking set this needed a moment ago was a symptom of shared state. */
    void sendFmMetaAll() { for (auto& p : allSpecPeers()) sendFmMeta(p.sock); }

    bool sendFmMeta(const std::shared_ptr<net::Socket>& sock) {
        std::string ps, rt; int pi = -1, ecc = 0, ber = -1; float sig = -99.0f;
        // ★★★ THIS LISTENER'S MODE, not the shared one. `mode` is the shared pipeline's, which in
        //     per-client mode is whatever the server started in — so a listener who tuned WFM
        //     themselves was judged "not FM" and the whole RDS message was suppressed. RDS,
        //     Advanced RDS and the stereo flag all vanished together, which is exactly how it
        //     presented (Stuart, 2026-08-05).
        auto c = dspFor(sock);
        // ★ THIS listener's state, falling back to the server's own only where there is no
        //   per-client pipeline at all (the phone and Mac apps, one listener, no ambiguity).
        // ★ Read from whoever is decoding THIS listener's frequency — themselves, or the listener
        //   who claimed it. Same carrier, same data; see rdsFreqOwner.
        auto src = rdsSourceFor(c);
        RdsState& R = src ? src->rdsS : (c ? c->rdsS : rdsS);
        // ★★★ STEREO IS THIS LISTENER'S OWN, ALWAYS. It is the 19 kHz PILOT, which every WFM
        //     pipeline locks as part of producing stereo audio — it costs nothing extra and is
        //     never shared. Reading it from the RDS source meant a listener who was not the RDS
        //     owner had no stereo icon while plainly HEARING stereo (Stuart, 2026-08-05: "I can
        //     hear the stereo working but no icon for it"). The share is for the RDS DEMOD, which
        //     is expensive and identical for everyone on a carrier; the pilot is neither.
        RdsState& S = c ? c->rdsS : rdsS;
        bool wfm = c ? (c->mode == "wfm") : (mode == "wfm");
        if (wfm) {
            std::lock_guard<std::mutex> lk(R.rdsMtx);
            ps = R.rdsPsName; rt = R.rdsText; pi = R.rdsPi; ecc = R.rdsEcc; ber = R.rdsBer; sig = R.rdsSig;
        }
        // trim trailing spaces RDS pads with
        auto trim = [](std::string s){ size_t e = s.find_last_not_of(" \t\r\n"); return e==std::string::npos?std::string():s.substr(0,e+1); };
        ps = trim(ps); rt = trim(rt);
        const bool st = wfm && S.stereoDetected.load();
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
        if (ps == R.lastSentPs_ && rt == R.lastSentRt_ && pi == R.lastSentPi_ && ecc == R.lastSentEcc_
            && st == R.lastSentStereo_ && ber == R.lastSentBer_
            && std::fabs(sig - R.lastSentSig_) < 0.5f) return false;
        R.lastSentPs_ = ps; R.lastSentRt_ = rt; R.lastSentPi_ = pi; R.lastSentEcc_ = ecc; R.lastSentStereo_ = st;
        R.lastSentBer_ = ber; R.lastSentSig_ = sig;
        char buf[512];
        snprintf(buf, sizeof buf,
            "{\"type\":\"rds\",\"stereo\":%s,\"ps\":\"%s\",\"radiotext\":\"%s\",\"pi\":%d,\"ecc\":%d,\"ber\":%d,\"sig\":%.1f}",
            st ? "true" : "false",
            jsonEscape(ps).c_str(), jsonEscape(rt).c_str(), pi, ecc, ber, sig);
        sendText(sock, buf);
        return true;
    }

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
        // Guard band before we recentre the capture. The fixed 50 kHz was fine at
        // 2.4 MSPS but goes NEGATIVE once decimation makes the IQ narrow (SpyServer),
        // which would retune on every tiny nudge.
        double limit = sampleRate / 2.0 - std::min(50000.0, sampleRate * 0.15);

        // ★★★ LOCKED CENTRE: the capture window is the fixed thing, so CLAMP the VFO into it
        // rather than moving the radio. On a shared receiver a retune past the edge would slide
        // the band under every other listener mid-sentence; inside the window everyone tunes
        // freely, which is the whole point of capturing 8 MHz. Clamped, not refused — a client
        // that asks for 14 MHz on a 2.5-10.5 window gets the edge, not silence.
        const double lockC = g_vsLockedCentre.load();
        if (lockC > 0.0) {
            const double lo = lockC - limit, hi = lockC + limit;
            if (freq < lo || freq > hi) {
                LOGI("retune %.3f MHz is outside the LOCKED window %.3f-%.3f MHz — clamped",
                     freq / 1e6, lo / 1e6, hi / 1e6);
                freq = freq < lo ? lo : hi;
            }
            audioFreq.store(freq);
            rx.setTune(vfoOffsetNow(), rxMode, rxBwHz);
            { std::lock_guard<std::mutex> rl(rdsS.rdsMtx); rdsS.rdsPsName.clear(); rdsS.rdsText.clear(); rdsS.rdsPi = -1; }
            rdsS.stereoDetected.store(false);
            // ★★★ AND ASK FOR IT BACK. The stereo report is EDGE-triggered and a retune inside the
            //     same mode does not rebuild the chain, so the pilot never unlocks and there is no
            //     edge to refill what we just cleared — the listener sat in mono on a plainly stereo
            //     station until something forced a rebuild (Stuart, 2026-08-08). Measured on the Pi:
            //     the DSP was locked with blend=1.00 the whole time; only the REPORT was missing.
            rx.requestStereoReport();
            // ★ And drop what the decoder learned from the PREVIOUS station — its timing hypothesis
            //   scores outlive a retune, and a stale one beats the correct one on a weak signal.
            rx.requestRdsResync();
            return;
        }

        // ★★★ THE OWNER'S LIMITS ARE ENFORCED HERE, NOT ONLY IN THE BROWSER. The client bounces and
        //     jumps at the edges because that feels right; this exists because a permitted set that
        //     lives only in the page is decoration — a hand-rolled client can send any frequency it
        //     likes, and "we asked the browser nicely" is not a limit an owner can rely on for a
        //     band they are blocking for legal reasons. Same split as the locked window above.
        // ★ Clamped rather than refused, matching the locked window: a listener is put at the edge
        //   of what they may hear, not left in silence wondering.
        {
            vibebands::Ranges hw;
            const vibebands::Ranges perm = vsPermittedRanges(hw.empty() ? vibebands::Ranges{{0.0, 2.0e9}} : hw);
            if (!perm.empty() && !vibebands::allows(perm, freq)) {
                const double was = freq;
                freq = vibebands::clamp(perm, freq);
                LOGI("retune %.3f MHz is outside the permitted bands — moved to %.3f MHz",
                     was / 1e6, freq / 1e6);
            }
        }

        audioFreq.store(freq);
        // ★★★ TUNING INTO A CAPPED BAND MUST BRING THE GAIN DOWN WITH IT. Without this the whole
        //     feature is walk-aroundable in the most ordinary way possible: set maximum gain on
        //     HF, where the owner allows it, then tune to Broadcast FM — and the front end is
        //     overloaded by exactly the setting the cap exists to prevent, having passed every
        //     check on the way in. Stuart, 2026-08-12: "if I tune from a band which has a higher
        //     limit or unlimited set and move into a limited band then we need to reduce the gain
        //     to the limit set automatically."
        // ★★ Only ever DOWNWARD. Leaving a band does not restore what the listener had before —
        //    that would mean remembering a value they may never have chosen, and quietly RAISING
        //    the gain is the one direction that can damage nothing but sound like everything.
        applyGainCapForFreq(freq);
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
        { std::lock_guard<std::mutex> rl(rdsS.rdsMtx); rdsS.rdsPsName.clear(); rdsS.rdsText.clear(); rdsS.rdsPi = -1; }
        rdsS.stereoDetected.store(false);
        // ★★★ AND ASK FOR IT BACK. The stereo report is EDGE-triggered and a retune inside the
        //     same mode does not rebuild the chain, so the pilot never unlocks and there is no
        //     edge to refill what we just cleared — the listener sat in mono on a plainly stereo
        //     station until something forced a rebuild (Stuart, 2026-08-08). Measured on the Pi:
        //     the DSP was locked with blend=1.00 the whole time; only the REPORT was missing.
        rx.requestStereoReport();
        // ★ And drop what the decoder learned from the PREVIOUS station — its timing hypothesis
        //   scores outlive a retune, and a stale one beats the correct one on a weak signal.
        rx.requestRdsResync();
    }

    // ── WebSocket framing ──────────────────────────────────────────────────
    /** Frame and QUEUE one WebSocket message. Returns immediately — see the Outbox block above.
     *  ★★ HEADER AND PAYLOAD ARE ONE BUFFER, not two sends. The old code wrote them separately
     *  under a lock; queued separately they could be interleaved by anything that ever pushed
     *  between the two, and a WebSocket header split from its body desynchronises the stream for
     *  good. One frame in, one frame out — the invariant is now structural rather than a lock. */
    void sendWs(const std::shared_ptr<net::Socket>& sock, uint8_t opcode,
                const uint8_t* payload, size_t len, Out cls = Out::Control) {
        if (!sock || !sock->isOpen()) return;
        std::vector<uint8_t> f;
        f.reserve(len + 10);
        f.push_back(0x80 | opcode);
        if (len < 126) f.push_back((uint8_t)len);
        else if (len < 65536) { f.push_back(126); f.push_back((uint8_t)(len>>8)); f.push_back((uint8_t)len); }
        else { f.push_back(127); for (int i=7;i>=0;i--) f.push_back((uint8_t)(len>>(i*8))); }
        if (len) f.insert(f.end(), payload, payload + len);
        outboxPush(sock, cls, std::move(f));
    }
    void sendText(const std::shared_ptr<net::Socket>& sock, const std::string& s,
                  Out cls = Out::Control) {
        sendWs(sock, 0x1, (const uint8_t*)s.data(), s.size(), cls);
    }
    void sendConfig(const std::shared_ptr<net::Socket>& sock) {
        // NB: displaySpan(), not sampleRate. On SpyServer the waterfall is the
        // server's wide FFT while the IQ is narrow, so the client's zoom/pan model
        // must be built on the span it can actually SEE.
        const double span = displaySpan();
        double effective = span / zoomFactor.load();                  // zoom-aware span
        // ★★★ THIS CLIENT'S width, not a global. sendConfig tells the client the bin count its
        //     whole zoom/pan model is built on — handing it someone else's is the "derive a wire
        //     value the same way at BOTH ends" bug, and it lands every zoom off by the ratio.
        const int cfgBins = binsFor(sock);
        std::string myMode = mode;
        double myVfo = audioFreq.load();
        double myCentre = viewCenter.load(), myEffective = effective;
        if (auto me = dspFor(sock)) {
            myMode = me->mode; myVfo = me->vfoHz;
            // ★ Its own view, or the shared one — the client scales its axis by what we say here,
            //   so telling it the shared span while sending it a private zoomed row draws every
            //   signal in the wrong place.
            // ★ Whenever this listener has a view of its own — whether it is served from its
            //   private channel or as a crop of the shared row. The client scales its axis by
            //   what we say here, so reporting the global span while sending it a cropped frame
            //   draws every signal in the wrong place. Gating this on ownView was why the zoom
            //   appeared to have only two positions.
            if (me->viewSpanHz > 0) { myCentre = me->viewCentreHz; myEffective = me->viewSpanHz; }
        }
        const double binBw = myEffective / (double)cfgBins;   // we emit cfgBins bins over MY span
        char buf[512];   // grew when vfo/locked were added — a truncated JSON config is fatal
        // maxBandwidth = full (unzoomed) device span — the client caps zoom-out
        // to this so you can't zoom out past the actual RTL bandwidth.
        // ★ mode: the server is AUTHORITATIVE on its own starting demodulator (the owner sets it,
        // and it is configurable). Without it the web client defaulted to nfm while the server ran
        // wfm — the UI showed NFM with a thin NFM passband until you clicked a mode. The client
        // adopts this on the first config when it has no remembered session.
        snprintf(buf, sizeof buf,
            // ★★★ `vfo` — WHERE THE RADIO IS ACTUALLY TUNED, and the client was never told.
            //     Without it a joining listener has no way to know, so it falls back on its own
            //     remembered frequency and immediately tunes away from where the server put it.
            //     That is what made the owner's landing frequency look ignored: the server DID
            //     land on 7074, and the client moved straight off it (Stuart, 2026-08-05).
            //     ★ Same reasoning as `mode` two fields along, which already exists for exactly
            //       this: the server is authoritative about its own radio.
            //     ★★ It matters most on a SHARED receiver, where there is one VFO and a joiner
            //        must adopt it rather than impose one — otherwise the last person to connect
            //        silently retunes the radio for everybody already listening.
            "{\"type\":\"config\",\"centerFreq\":%lld,\"binCount\":%d,"
            "\"binBandwidth\":%.6f,\"totalBandwidth\":%.1f,\"maxBandwidth\":%.1f,"
            "\"mode\":\"%s\",\"vfo\":%lld,\"locked\":%s}",
            (long long)llround(myCentre), cfgBins, binBw, myEffective, span,
            // ★★ THIS listener's mode and VFO, not the server's. In shared mode they are
            //    genuinely different per listener, and telling a client someone else's dial is
            //    how it ends up tuned somewhere it never asked for.
            myMode.c_str(), (long long)llround(myVfo),
            g_vsLockedCentre.load() > 0.0 ? "true" : "false");
        sendText(sock, buf);
    }

    // Waterfall zoom: set the FFT-crop factor to match the requested span
    // (binBandwidth*fftSize). Pure display-side crop in onFFT — no IQ
    // decimation, no IQFrontEnd reconfig (which would touch the uninitialised
    // headless core), no effect on audio. Capped so the crop keeps >= 16 bins.
    /** ★★★ `clientBins` IS THE SENDER'S OWN WIDTH, and it must be. The client expresses the span
     *  it wants as binBandwidth x (the bins IT receives); interpreting that with anyone else's
     *  count scales every zoom by exactly the ratio between them — 32x for a watch against a
     *  desktop. Same shape as "derive a wire value the same way at BOTH ends". */
    void setSpan(double binBw, int clientBins) {
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
        // ★★★ displaySpan(), NOT sampleRate — THE SAME SPAN sendConfig USED TO DERIVE binBw.
        // sendConfig sends the client `displaySpan() / zoom / bins`; if this reverses it with
        // sampleRate instead, the two directions disagree by exactly the dead-lobe crop and every
        // round trip multiplies the zoom by 912/768 = 1.1875. The client re-asserts its view while
        // you hold the tune button, so it RATCHETS IN ~19% a time — Stuart, 2026-08-02: "when
        // tuning on fm using 100KHz steps as the tuning happens by holding the tune button it also
        // zooms in too", and then, correctly, "our dead space crop?".
        // ★★ It was right by ACCIDENT until this morning: displaySpan() simply RETURNED sampleRate
        // on every path that reaches here, so the mismatch had no effect and nothing marked the two
        // as a pair that must agree. The crop (23868d1) separated them and the latent bug woke up.
        // The note ten lines above warns about precisely this shape for the BIN COUNT — "Both sides
        // must use the SAME bin count". Same bug, one field along.
        // ★ NOT filter-related: it reproduces on NFM, which is what ruled out the demod-bandwidth
        // clearance and pointed here.
        double want = displaySpan() / (binBw * (double)clientBins);
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
        updateZoomView();
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
        // ★★ THE IDLE CLOCK IS STAMPED HERE, AT THE TOP OF EVERY CONTROL MESSAGE — see
        //    enforceAdminIdle(). Every message that arrives on this path is something a client
        //    ASKED FOR, which is as close to "a human is present" as a server can honestly get;
        //    the frames going the other way keep flowing to an empty room forever.
        //    ★ Deliberately not filtered to admin-only commands: tuning, zooming and changing
        //      mode are all evidence that somebody is there, and re-locking an admin who is
        //      plainly using the receiver would be a bug wearing a security feature's clothes.
        touchAdmin(sock);
        // ★★★ PER-CLIENT CONTROL COMES FIRST, AND THE ORDER IS LOAD-BEARING. The shared handlers
        //     below (zoom, reset, tune…) return as soon as they match, so a per-client block
        //     placed after them is dead code for every message they claim. It was, for zoom: one
        //     listener zooming moved the GLOBAL zoom, and the others only looked unaffected
        //     because nothing re-sent them a config — their waterfall was quietly rescaled
        //     underneath them while they were told the old span.
        // ★★★ IN SHARED MODE, TUNING IS THE SENDER'S OWN BUSINESS. This is the whole point of
        //     per-client DSP: the message moves THIS listener's VFO and nobody else's. Before
        //     this, one window tuning retuned every other listener on the server.
        //     ★ The hardware centre still never moves — it is locked, and everyone's channel is
        //       taken from inside that one capture.
        if (auto me = dspFor(sock)) {
            // ★★ ZOOM IS PER LISTENER TOO. Whether it is served from this client's own channel or
            //    from the shared wide waterfall is decided HERE, by whether the requested span is
            //    narrow enough to be worth a private view — the same handover rule the wide path
            //    already uses internally (zoomed past real resolution ⇒ the narrow path wins).
            if (type == "zoom" || type == "reset") {
                double v = 0, bb = 0;
                if (type == "reset") { me->viewSpanHz = 0; me->ownView = false; }
                else {
                    if (jsonNum(msg, "frequency", v) && v > 0) me->viewCentreHz = v;
                    if (me->viewCentreHz <= 0) me->viewCentreHz = me->vfoHz;
                    if (jsonNum(msg, "binBandwidth", bb) && bb > 0)
                        me->viewSpanHz = bb * (double)binsFor(sock);
                    // ★★★ THE HANDOVER IS EXACTLY WHERE THE SHARED FFT RUNS OUT OF REAL BINS,
                    //     and nowhere else. `perOutBin < sharedBinHz` IS the brief's step < 1.0:
                    //     below it the wide row is being stretched and we are magnifying nothing.
                    //     ★ It used to ALSO require the view to fit the AUDIO channel, which on AM
                    //       meant 25 kHz — so every zoom between 25 and 250 kHz got the stretched
                    //       wide row and looked blocky, which is most of the useful range. The
                    //       view has its own channel now, sized to the view, so that condition is
                    //       gone and the sharp path starts the moment it is worth having.
                    const double sharedBinHz = sampleRate / (double)fftSize;
                    const double perOutBin   = me->viewSpanHz / (double)binsFor(sock);
                    me->ownView = me->viewSpanHz > 0 && perOutBin < sharedBinHz;
                }
                clientRetune(me.get());
                sendConfig(sock);
                return;
            }
            if (type == "tune" || type == "mode" || type == "bandwidth") {
                std::string m = jsonStr(msg, "mode");
                double v = 0, lo = 0, hi = 0, bw = 0;
                bool changed = false;
                if (!m.empty() && m != me->mode) { me->mode = m; me->bwHz = paramsFor(m).bandwidth; changed = true; }
                if (jsonNum(msg, "frequency", v) && v > 0) {
                    // ★ Clamp to the captured window: a listener cannot tune outside the range
                    //   the owner locked, and silently accepting it would demodulate noise.
                    const double half = displaySpan() * 0.5;
                    const double lo2 = rtlCenter.load() - half, hi2 = rtlCenter.load() + half;
                    // ★★★ BUT ON A RADIO THAT MAY MOVE, CLAMPING IS THE BUG. This path was written
                    //     for the SHARED case, where the capture is fixed and everyone tunes inside
                    //     it — and it was then used for every radio. So a lone listener restoring
                    //     99.7 MHz on a receiver parked at 648 kHz had their VFO pinned to the edge
                    //     of the old window: the app showed 99.7, the spectrum drew a black band,
                    //     and the audio was still Caroline on 648 (Stuart, 2026-08-16). One click
                    //     of the dial "fixed" it — because by then something else had moved the
                    //     radio, so the same request now fell inside the window.
                    // ★★ The shared `retune()` already knows how to do this properly: it moves the
                    //    capture, re-checks the OWNER'S PERMITTED BANDS, and brings the gain down
                    //    if the new band is capped. Reimplementing any of that here would be a
                    //    second set of rules to keep in step, so ask it instead.
                    // ★★ Only when the centre is not locked AND we are the only listener. Moving
                    //    the capture under other people mid-sentence is exactly what the clamp
                    //    exists to prevent, and that reasoning is still right for a shared radio.
                    // ★ No lock is held here — dspFor() released clientMtx before returning — so
                    //   taking modeMtx inside retune() is the same order the zoom handler uses.
                    if ((v < lo2 || v > hi2)
                        && g_vsLockedCentre.load() <= 0.0 && specListenerCount() <= 1) {
                        retune(v);
                        me->vfoHz = audioFreq.load();   // whatever retune() actually settled on
                    } else {
                        me->vfoHz = v < lo2 ? lo2 : (v > hi2 ? hi2 : v);
                    }
                    changed = true;
                }
                if (jsonNum(msg,"bandwidthLow",lo) && jsonNum(msg,"bandwidthHigh",hi)) { me->bwHz = hi - lo; changed = true; }
                else if (jsonNum(msg,"bandwidth",bw) && bw > 0) { me->bwHz = bw; changed = true; }
                if (changed) clientRetune(me.get());
                return;
            }
            // ★★★ THIS LISTENER'S OWN AUDIO EFFECTS. Falling through to the shared handlers below
            //     set them on a pipeline that feeds nobody in per-client mode — the control moved
            //     and the audio did not change. See the note on ClientDsp's effect state.
            //     ★ Deliberately NOT admin-gated, unlike gain: these change only what THIS
            //       listener hears, so there is nothing shared to protect.
            if (type == "nr") {
                me->nrOn.store(msg.find("\"on\":true") != std::string::npos);
                double st;
                if (jsonNum(msg, "strength", st)) {
                    me->nrStrength = (float)std::max(0.0, std::min(1.0, st));
                    std::lock_guard<std::mutex> lk(me->fxMtx);
                    if (me->nrEng) me->nrEng->setStrength(me->nrStrength);
                }
                if (!me->nrOn.load()) { std::lock_guard<std::mutex> lk(me->fxMtx);
                                        if (me->nrEng) me->nrEng->reset(); }
                return;
            }
            if (type == "notch") {
                me->notchOn.store(msg.find("\"on\":true") != std::string::npos);
                if (!me->notchOn.load()) { std::lock_guard<std::mutex> lk(me->fxMtx);
                                           if (me->notchEng) me->notchEng->reset(); }
                return;
            }
            if (type == "deemph") {
                double tau;
                // ★ SECONDS on the wire — see vibeserver_wire_units_seconds. A value in
                //   microseconds here is 1000x wrong and sounds like no de-emphasis at all.
                if (jsonNum(msg, "tau", tau) && me->rx) { me->deempTau = tau; me->rx->setDeemphasis(tau); }
                return;
            }
            if (type == "stereo") {
                const bool on = msg.find("\"on\":true") != std::string::npos;
                me->stereoOn.store(on);
                if (me->rx) me->rx->setStereoEnabled(on);
                return;
            }
        }

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
        auto adminGate = [this, &sock](const char* what) -> bool {
            bool needed;
            { std::lock_guard<std::mutex> lk(g_vsAdminMtx); needed = !g_vsAdminSecret.empty(); }
            if (adminNow(sock)) return true;
            // ★★★ NO PASSWORD SET MEANS NOBODY IS AUTHORISED — NOT THAT EVERYONE IS.
            //
            // This used to be `if (!needed || adminOk)`, so a receiver with no admin password let
            // EVERY listener change the gain, the calibration, direct sampling and BIAS-T — which
            // puts DC on the feedline. The reasoning was that a host who never asked for a
            // password should not find their own controls refusing to work, and that is right —
            // but the host is LOOPBACK, and this let the whole network in to protect them.
            //
            // ★★ So: with no password, the machine running the server keeps every control and
            //    nobody else gets any. Zero friction for plug-and-play (there is still nothing to
            //    type), and a radio that is accidentally exposed cannot be damaged by a stranger.
            //    Silence is not consent (Stuart asked the right question, 2026-08-07: "do we force
            //    an admin password in simple mode to prevent a user accidentally exposing a radio
            //    with full admin controls?" — forcing one taxes the flow; this does not).
            // ★ A headless Linux server is unaffected: its wizard makes the admin password
            //   mandatory, so `needed` is always true there and this branch never applies.
            if (!needed && sock && isLoopback(sock->peerAddress())) return true;
            LOGI("refused %s — %s", what,
                 needed ? "admin password required"
                        : "no admin password is set, so only this machine may change the radio");
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
            setAdminNow(sock, ok);
            // ★ Start (or restart) the idle clock the moment admin is granted, so a session
            //   that is unlocked and then never touched still re-locks on schedule.
            // (setAdminNow stamps the idle clock for this listener.)
            LOGI("admin unlock %s", ok ? "granted" : "REFUSED");
            std::shared_ptr<net::Socket> sc;
            { std::lock_guard<std::mutex> lk(clientMtx); sc = specClient; }
            if (sc) sendText(sc, ok ? "{\"type\":\"admin\",\"ok\":true}"
                                    : "{\"type\":\"admin\",\"ok\":false}");
            return;
        }
        // ★★★ ON A SHARED RECEIVER, NOTHING SHARED IS UNLOCKED. The radio's front end and the
        //     server-side DSP are ONE set of settings serving everybody: a listener changing the
        //     gain, the squelch, NR, the notch, de-emphasis or stereo changes them for every other
        //     listener, not for themselves (Stuart, 2026-08-03: "no controls unlocked at all in
        //     shared user mode"). Behind the admin password with bias-T and calibration.
        //     ★ Gated on the LOCK, not always: on a personal receiver these are the owner's own
        //       radio and must stay free — a phone user unlocking a password to touch their own
        //       dongle would be an obstacle, not protection.
        //     ★ What stays FREE even here, because it is genuinely per-listener: tune (clamped to
        //       the window), mode, bandwidth, zoom/pan and the frame rate.
        auto sharedGate = [&](const char* what) -> bool {
            return g_vsLockedCentre.load() <= 0.0 || adminGate(what);
        };
        if (type == "ahf_control") {
            if (!sharedGate("Airspy HF+ controls")) return;
            // ★★★ AGC LOCKED MEANS THE MANUAL PATH IS CLOSED TOO. The attenuator and the preamp
            //     ARE this radio's gain — leaving them writable while the AGC is "locked" would be
            //     a lock in name only, since a listener could simply attenuate or boost around it.
            //     ★ The threshold below is the AGC's own setting, not a way past it, so it stays.
            const bool ahfManualLocked = LocalSdrShim::agcLocked();
            if (jsonNum(msg, "att", v)) {
                if (ahfManualLocked) LOGI("attenuator refused — the owner has locked the AGC on");
                else                 LocalSdrShim::instance().setAhfAttenuation((int)v);
            }
            if (jsonNum(msg, "lna", v)) {
                if (ahfManualLocked) LOGI("preamp refused — the owner has locked the AGC on");
                else                 LocalSdrShim::instance().setAhfLna(v != 0);
            }
            if (jsonNum(msg, "thresh", v)) LocalSdrShim::instance().setAhfAgcThreshold(v != 0);
            // ★ Calibration is protected even though the rest of this message is not:
            // gain a listener can play with, a miscalibrated reference they cannot see.
            if (jsonNum(msg, "ppb", v) && adminGate("calibration")) {
                LocalSdrShim::instance().setAhfCalibrationPpb((int)v);
                vsPersist("{\"ppb\":" + std::to_string((int)v) + "}");
            }
            // ★ AGC LAST, matching the client's own send order: it owns the gain path, so
            // applying it before the manual attenuation would let it immediately override it.
            // ★★★ ON THE HF+ THE AGC LOCK IS THE WHOLE FEATURE. This radio has no variable gain to
            //     cap — an 8-step attenuator and a preamp — and its AGC is trusted ("the AGC on
            //     that is good enough", Stuart), so "protect the front end" here means: leave it
            //     to the radio, and do not let a listener take it off.
            if (jsonNum(msg, "agc", v)) {
                if (LocalSdrShim::agcLocked() && v == 0) LOGI("AGC off refused — locked by the owner");
                else                                     LocalSdrShim::instance().setAhfAgc(v != 0);
            }
            return;
        }
        if (type == "rsp_control") {
            // ★★★ GAIN IS SHARED HARDWARE ON A SHARED RECEIVER. One listener moving the LNA or the
            //     IF reduction moves it for EVERYONE — it is the front end, not a per-listener
            //     preference — so on a LOCKED receiver it belongs behind the admin password with
            //     bias-T and calibration (Stuart, 2026-08-03). On a personal receiver it is the
            //     owner's own radio and stays freely adjustable, which is why this is gated on the
            //     lock rather than always: a control that a phone user must unlock to use their
            //     own dongle would be an obstacle, not protection.
            if (!sharedGate("gain")) return;
            // ★★★ AND REMEMBER IT. An admin who unlocked these and moved them did so deliberately
            //     — "I have used the admin password to unlock them, so it is safe to say I'm the
            //     admin and I've changed the controls for a reason" (Stuart, 2026-08-05). Losing it
            //     on the next restart brings the receiver back overloaded, silently, and the owner
            //     has to spot it and do it again. Saved WITHOUT a restart: this is a live nudge,
            //     not a setup change.
            // ★★★ SAVE BOTH REPRESENTATIONS, OR THE SETTING REVERTS ON EVERY RESTART. The RF gain
            //     exists twice in the config: `lnaState` (what this control sets) and `gain` (the
            //     generic tenth-dB slider every driver shares). START-UP READS `gain` — open()
            //     calls setGainTenthDb(), which DERIVES the LNA state from it and overwrites
            //     whatever `lnaState` said. So an admin could set the RF gain, watch it take
            //     effect, and find it back at the old value after the next restart, with the
            //     config file showing the value they chose (Stuart, 2026-08-10: "it had dropped
            //     back to 2/9 when it was on 7/9").
            // ★★★ AND IT WAS NOT MERELY ANNOYING — IT BRICKED THE RECEIVER. The stale `gain: 110`
            //     re-derived LNAstate 7, which together with the gRdB 59 seeded before Init is a
            //     combination the API rejects outright: Init failed, the process aborted, systemd
            //     restarted it for ever, and no reboot could clear it because the bad number was
            //     in the file. Two fields for one quantity, and the one nobody was looking at won.
            // ★★ Write the EQUIVALENT gain, inverting setGainTenthDb's own mapping
            //    (st = (1 - tenthDb/490) * (n-1)), so the two cannot disagree again. Derived from
            //    the live ladder length, so an RSP1 and an RSPdx each get their own answer.
            // ★ Belt and braces: setLnaState() below still applies the state directly, so the
            //   LIVE gain is exactly what was asked for even if the arithmetic here rounds.
            // ★★★ THE OWNER'S RF CEILING — AND THE NUMBER ON THE WIRE COUNTS THE OTHER WAY.
            //     `lna` is the RAW LNA STATE, where HIGHER means MORE attenuation and therefore
            //     LESS gain; the owner's cap is a GAIN POSITION, where higher means more gain (see
            //     BRIEF-admin-gain-limits.md). Converted once, here, against the LIVE ladder — an
            //     RSP1 has 4 states and an RSPdx 28, so the arithmetic cannot be hard-coded.
            //     ★★★ GET THIS BACKWARDS AND THE FEATURE DOES THE OPPOSITE OF WHAT WAS ASKED: an
            //         owner capping FM would be FORCING the radio to maximum RF gain on exactly
            //         the band that was overloading, and it would look like it was working,
            //         because a number went in and a number came out.
            // ★★ RF, not IF, deliberately: the LNA sits ahead of the mixer, so it is what decides
            //    whether the front end overloads at all — and capping it leaves the IF AGC its
            //    full range, so there is no clamp for the AGC to fight.
            if (jsonNum(msg, "lna", v)) {
                const int cap = LocalSdrShim::gainCapAt(LocalSdrShim::instance().listenFrequency());
                const int n = (useSdrplay() && sdrp) ? sdrp->lnaStateCount() : 0;
                if (cap >= 0 && n > 1) {
                    const int minState = std::max(0, (n - 1) - cap);   // more gain = lower state
                    if ((int)v < minState) {
                        LOGI("RF gain state %d raised to %d by the owner's limit (cap position %d)",
                             (int)v, minState, cap);
                        v = minState;
                    }
                }
            }
            if (jsonNum(msg, "lna", v))    { LocalSdrShim::instance().setLnaState((int)v);
                                             const int n = (useSdrplay() && sdrp) ? sdrp->lnaStateCount() : 0;
                                             std::string j = "{\"lnaState\":" + std::to_string((int)v);
                                             if (n > 1) {
                                                 int st = (int)v; if (st < 0) st = 0; if (st > n - 1) st = n - 1;
                                                 const int tenth = (int)llround(490.0 * (1.0 - (double)st / (double)(n - 1)));
                                                 j += ",\"gain\":" + std::to_string(tenth > 0 ? tenth : 1);
                                             }
                                             vsPersist(j + "}"); }
            if (jsonNum(msg, "ifgr", v))   { LocalSdrShim::instance().setIfGainReduction((int)v);
                                             vsPersist("{\"ifGr\":" + std::to_string((int)v) + "}"); }
            // ★★★ AGC LOCKED = THE LISTENER MAY NOT TURN IT OFF. The owner has decided the radio's
            //     own loop keeps the front end safe; letting a visitor switch to manual would hand
            //     them the very control the lock exists to withhold. Turning it ON is always
            //     allowed — that is moving TOWARDS what the owner asked for.
            if (jsonNum(msg, "ifagc", v)) {
                if (LocalSdrShim::agcLocked() && v == 0) {
                    LOGI("AGC off refused — the owner has locked it on");
                } else {
                    LocalSdrShim::instance().setIfAgc(v != 0);
                    vsPersist(std::string("{\"ifAgc\":") + (v != 0 ? "1" : "0") + "}");
                }
            }
            if (jsonNum(msg, "agcset", v))   LocalSdrShim::instance().setIfAgcSetPoint((int)v);
            // Loop dynamics arrive together — they only make sense as a set.
            {
                double a, d, dd, th;
                if (jsonNum(msg, "agcAttack", a) && jsonNum(msg, "agcDecay", d)
                 && jsonNum(msg, "agcDelay", dd) && jsonNum(msg, "agcThresh", th))
                    LocalSdrShim::instance().setIfAgcDynamics((int)a, (int)d, (int)dd, (int)th);
            }
            if (jsonNum(msg, "rfnotch", v))  { LocalSdrShim::instance().setRfNotch(v != 0);
                                               vsPersist(std::string("{\"rfNotch\":") + (v != 0 ? "true" : "false") + "}"); }
            if (jsonNum(msg, "dabnotch", v)) { LocalSdrShim::instance().setDabNotch(v != 0);
                                               vsPersist(std::string("{\"dabNotch\":") + (v != 0 ? "true" : "false") + "}"); }
            // ★ The RSP has its own bias-T, and it is the same hazard as the dongle's.
            if (jsonNum(msg, "biast", v) && adminGate("bias-T"))
                LocalSdrShim::instance().setBiasT(v != 0);
            return;
        }
        if (type == "reset") { zoomFactor.store(1.0); updateZoomView(); sendConfig(sock); return; }
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
                    ? bb * (double)binsFor(sock)        // same reason as setSpan()
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
            if (jsonNum(msg,"binBandwidth",bb) && bb > 0) setSpan(bb, binsFor(sock));
            // A PAN with no span change never reaches setSpan(), but it moves the view centre —
            // and the zoom channel is tuned to that centre, so it has to follow.
            else updateZoomView();
            sendConfig(sock);
            return;
        }
        if (type == "tune") {
            // ★★★ REMEMBER THAT THIS SESSION HAS PLACED ITSELF, so the landing gate does not
            //     later overrule it — see preTunedSession. Keyed off sockSession, NOT pendingAudio:
            //     the latter exists only on a per-client radio, and the receiver that reported this
            //     bug is single-listener, where perClientDsp() is false and none of that code runs.
            {
                std::lock_guard<std::mutex> lk(clientMtx);
                auto it = sockSession.find(sock.get());
                if (it != sockSession.end() && !it->second.empty()) preTunedSession = it->second;
            }
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
            // Same rule as rsp_control above: the front end is shared, so a locked receiver puts it
            // behind the admin password; a personal one leaves it alone.
            if (!sharedGate("gain")) return;
            if (msg.find("\"auto\":true") != std::string::npos) {
                LocalSdrShim::instance().setGain(-1); vsPersist("{\"gain\":-1}");
            } else if (jsonNum(msg,"value",v)) {
                // ★★★ THE OWNER'S CEILING FOR THIS BAND. A ceiling, not a lock: the listener keeps
                //     the control and simply cannot take it past what the owner allows here —
                //     which on Broadcast FM is what stops a strong local transmitter overloading
                //     the front end for everybody (Stuart, 2026-08-12).
                // ★★ Clamped rather than refused. A rejected command leaves the slider showing a
                //    value the radio is not using, which reads as a broken control; a clamp plus
                //    the corrected value below reads as a rule.
                const int cap = LocalSdrShim::gainCapAt(LocalSdrShim::instance().listenFrequency());
                int want = (int)v;
                if (cap >= 0 && want > cap) {
                    LOGI("gain %d capped to %d by the owner's limit", want, cap);
                    want = cap;
                }
                LocalSdrShim::instance().setGain(want);
                vsPersist("{\"gain\":" + std::to_string(want) + "}");
            }
            return;
        }
        if (type == "biasT") {
            if (!adminGate("bias-T")) return;
            const bool on = msg.find("\"on\":true") != std::string::npos;
            LocalSdrShim::instance().setBiasTee(on);
            // ★★★ AND WRITE IT DOWN. Applied to the hardware but never saved, so a restart put the
            //     DC back off — and an antenna that needs phantom power went dead with the setting
            //     still showing ON in the owner's UI (Stuart, 2026-08-08: "I had enabled the bias-t
            //     and was using it until you restarted it which should've saved the Bias-t
            //     setting"). Gain and the notches already did this; these four were missed.
            vsPersist(std::string("{\"biasT\":") + (on ? "true" : "false") + "}");
            return;
        }
        if (type == "agc") {
            // The AGC owns the gain, so it is the same shared front-end control by another name.
            if (!sharedGate("AGC")) return;
            const bool on = msg.find("\"on\":true") != std::string::npos;
            LocalSdrShim::instance().setAgc(on);
            vsPersist(std::string("{\"ifAgc\":") + (on ? "1" : "0") + "}");
            return;
        }
        if (type == "ppm") {
            if (!adminGate("ppm")) return;
            if (jsonNum(msg,"value",v)) { LocalSdrShim::instance().setPpm((int)v);
                                          vsPersist("{\"ppm\":" + std::to_string((int)v) + "}"); }
            return;
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
            if (jsonNum(msg,"value",v)) { LocalSdrShim::instance().setDirectSampling((int)v);
                                          vsPersist("{\"directSampling\":" + std::to_string((int)v) + "}"); }
            return;
        }
        // ── Audio DSP (squelch / NR / notch / de-emphasis / stereo) ───────────
        // These engines already run server-side; they were only reachable from
        // the on-device JS via JNI, so a REMOTE client (web or phone) had no way
        // to touch them. Exposing them here gives both remote clients the same
        // audio controls the local app has — and keeps the DSP server-side, so
        // the client stays a thin renderer (no duplicate DSP to drift).
        if (type == "squelch") {
            if (!sharedGate("squelch")) return;
            // db <= -100 means "off", matching the app's own convention.
            if (jsonNum(msg, "db", v))
                LocalSdrShim::instance().setSquelch(v > -100.0, (float)v);
            return;
        }
        if (type == "nr") {
            if (!sharedGate("noise reduction")) return;
            LocalSdrShim::instance().setNR(msg.find("\"on\":true") != std::string::npos);
            if (jsonNum(msg, "strength", v))
                LocalSdrShim::instance().setNrStrength((float)std::max(0.0, std::min(1.0, v)));
            return;
        }
        // ★★★ THE FOUR BROADCAST-FM TREATMENTS ARE PER LISTENER, and the premise that made them
        //     shared was simply wrong: "it is one DSP chain per radio" has not been true since
        //     every client got its own RxPipeline. The old code proved it — it had to LOOP over
        //     `clientDsp` forcing each listener's pipeline into step, which is work you only do
        //     when the state is genuinely separate.
        // ★★★ NOTHING SHARED IS AT STAKE. NR, IMS, CEQ and the blanker touch nobody's front end;
        //     they process this listener's audio and nothing else. Gain, the attenuator and the
        //     bias-T are different in kind — one aerial, one tuner — and stay behind the gate.
        //     Stuart, 2026-08-15: "yes make it per user."
        // ★★ THE PRECEDENT IS THE ADVANCED RDS RAW SWITCH, a few hundred lines up: "RAW is PER
        //    USER, PER SESSION. It changes only what this viewer is shown — so it cannot affect
        //    anyone else on the same receiver." Same argument, same conclusion.
        // ★ The server-wide values stay as the DEFAULT a new listener inherits, so the owner still
        //   sets the house behaviour in the config; a listener's toggle no longer writes it.
        //   A client with no pipeline of its own (nothing listening yet) is left alone rather than
        //   falling back to the global setter — silently changing everyone was the bug.
        // ★★★ AND IT MUST FALL BACK TO THE ONE PIPELINE. perClientDsp() is only true on a
        //     receiver with a LOCKED CENTRE and room for more than one listener — the shared RSP
        //     case. On an ordinary RTL or Airspy there is no per-client DSP at all, so looking one
        //     up and doing nothing when it is absent would have silently killed all four switches
        //     on the common setup while working perfectly on the one being tested.
        // ★★ On a single-user radio the shared pipeline IS this listener's, so the Impl's own
        //    atomics are updated with it and the state report below stays truthful.
        auto perListener = [&](void (vibedsp::RxPipeline::*setter)(bool),
                               std::atomic<bool> ClientDsp::*slot,
                               std::atomic<bool>* shared, bool on) {
            if (auto dsp = dspFor(sock)) {
                // ★ The value is REMEMBERED on the listener, not only pushed at the pipeline it
                //   happens to own right now — the next mode change replaces that pipeline.
                ((*dsp).*slot).store(on);
                if (dsp->rx) ((*dsp->rx).*setter)(on);
                return;
            }
            (rx.*setter)(on);
            if (shared) shared->store(on);
        };
        if (type == "wsp") {
            perListener(&vibedsp::RxPipeline::setWeakSignalProc, &ClientDsp::wspOn, &weakProcOn, msg.find("\"on\":true") != std::string::npos);
            return;
        }
        if (type == "nb") {
            perListener(&vibedsp::RxPipeline::setNoiseBlanker, &ClientDsp::nbOn, &nbOn, msg.find("\"on\":true") != std::string::npos);
            return;
        }
        if (type == "ceq") {
            perListener(&vibedsp::RxPipeline::setCeq, &ClientDsp::ceqOn, &ceqOn, msg.find("\"on\":true") != std::string::npos);
            return;
        }
        if (type == "ims") {
            perListener(&vibedsp::RxPipeline::setIms, &ClientDsp::imsOn, &imsOn, msg.find("\"on\":true") != std::string::npos);
            return;
        }
        if (type == "notch") {
            if (!sharedGate("the notch filter")) return;
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
            if (!sharedGate("de-emphasis")) return;
            // tau in SECONDS (0 = off, 50e-6 or 75e-6).
            if (jsonNum(msg, "tau", v)) LocalSdrShim::instance().setDeemphasis(v);
            return;
        }
        if (type == "stereo") {
            if (!sharedGate("stereo")) return;
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
                // ★★★ RECORD IT AGAINST THIS CLIENT, never in a global — the same rule as
                //     clientBins, and this is the line that broke it. It used to call
                //     setFftRate(), which sets the ENGINE rate for the whole radio, so one
                //     listener's idle-saver slowed every other listener's waterfall and left it
                //     slow after that listener had gone. See clientFps for the measurements.
                const double mr = g_vsMaxFftRate.load();     // the owner's ceiling, per client
                { std::lock_guard<std::mutex> lk(clientMtx);
                  clientFps[sock.get()] = (mr > 0 && v > mr) ? mr : v;
                  clientFpsAcc[sock.get()] = 0.0; }
                // The engine follows the FASTEST listener — this may raise it (a client asking
                // for more than anyone else) as well as lower it (the last slow one leaving).
                recomputeEngineRate();
                // A listener drawing its own zoomed view is fed by its own pipeline, whose rate
                // is set when that view is built — so rebuild it against the new rate.
                if (auto c = dspFor(sock)) if (c->ownView) clientRetuneView(c.get());
            }
            return;
        }
    }

    // VibeServer: advertise the tuner's supported gains to the client so its gain
    // slider has real dB steps (it can't query the remote device natively).
    /** ★★ Tell everyone how many are listening. Sent when the count CHANGES, not on a timer:
     *  it changes rarely, and a per-frame field on a hot path would be paying continuously for
     *  something that is news about twice an hour. */
    /** Safe to call during teardown — broadcastUsers() re-reads the lists, so a socket that has
     *  already been removed is simply not in them. */
    void broadcastUsersSafe() { broadcastUsers(); }

    void broadcastUsers() {
        const int n = specListenerCount();
        char b[96];
        snprintf(b, sizeof b, "{\"type\":\"users\",\"n\":%d,\"max\":%d}", n, g_vsMaxUsers.load());
        for (auto& p : allSpecPeers()) sendText(p.sock, b);
    }

    void sendUsers(const std::shared_ptr<net::Socket>& sock) {
        char b[96];
        snprintf(b, sizeof b, "{\"type\":\"users\",\"n\":%d,\"max\":%d}",
                 specListenerCount(), g_vsMaxUsers.load());
        sendText(sock, b);
    }

    void sendHwInfo(const std::shared_ptr<net::Socket>& sock) {
        std::vector<int> gains = LocalSdrShim::instance().getTunerGains();
        // ★★★ THE OWNER'S CEILING TRAVELS WITH THE HARDWARE INFO. A cap the client does not know
        //     about is a control that springs back: the listener drags the slider, the server
        //     quietly clamps it, and the UI shows a value the radio is not using — which reads as
        //     a broken receiver rather than as somebody's rule. Sent as the cap IN FORCE RIGHT
        //     NOW, at the frequency being listened to, so it follows the listener across bands.
        //     ★ -1 = no limit here, which is the overwhelmingly common answer and costs 8 bytes.
        //     ★ agcLocked says WHY the AGC switch will not move, so the client can show it locked
        //       rather than appear to ignore the tap.
        std::string j = "{\"type\":\"hwinfo\""
                      + std::string(",\"gainCap\":")
                      + std::to_string(LocalSdrShim::gainCapAt(
                            LocalSdrShim::instance().listenFrequency()))
                      + ",\"agcLocked\":" + (LocalSdrShim::agcLocked() ? "true" : "false")
                      // ★★★ WHERE THE GAIN ACTUALLY IS. Without it a remote client cannot know,
                      //     so both clients restored their OWN remembered gain on connect and
                      //     pushed it to the radio — which silently overrode the owner's resting
                      //     gain ("I set the RTL-SDR on the server to return to 12.5db but when I
                      //     opened it in the app it was at 29.7db", Stuart 2026-08-15) and, on a
                      //     SHARED receiver, re-gained it for everybody already listening.
                      // ★★ The comment that justified the push — "otherwise the slider shows a
                      //    value the radio isn't using" — was right about the problem and wrong
                      //    about which end should move. The radio is the authority on its own
                      //    gain; the slider follows it.
                      + ",\"gainNow\":" + std::to_string(
                            LocalSdrShim::instance().currentGainTenthDb())
                      + ",\"gains\":[";
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
        {
            std::string caps = LocalSdrShim::instance().radioCapsJson();
            // ★★★ THE OWNER'S LIMITS, PUBLISHED AS THE RADIO'S RANGES. The client already knows how
            //     to live inside a set of ranges — it is how the Airspy's 31-60 MHz hole works, and
            //     it gives the bounce-then-jump for free. Publishing the owner's permitted set the
            //     same way means one behaviour to reason about, not two.
            // ★★ The hardware's own ranges are the starting point, so a limit can never OFFER a
            //    frequency the radio cannot hear. Where a driver publishes none, its whole span is
            //    assumed and the owner's list is what narrows it.
            vibebands::Ranges hw;
            {
                const size_t k = caps.find("\"ranges\":[");
                if (k != std::string::npos) {
                    const size_t a = caps.find('[', k + 9), b = caps.find(']', a);
                    // "[[500,31000000],[60000000,260000000]]" — read the pairs back out.
                    const std::string body = caps.substr(k + 10, caps.find("]]", k) - k - 9);
                    size_t pos = 0;
                    while ((pos = body.find('[', pos)) != std::string::npos) {
                        const double lo = atof(body.c_str() + pos + 1);
                        const size_t c = body.find(',', pos);
                        const double hi = c == std::string::npos ? 0 : atof(body.c_str() + c + 1);
                        if (hi > lo) hw.push_back({ lo, hi });
                        pos = c == std::string::npos ? body.size() : c + 1;
                    }
                    (void)a; (void)b;
                }
            }
            if (hw.empty()) hw.push_back({ 0.0, 2.0e9 });
            const vibebands::Ranges perm = vsPermittedRanges(hw);
            // ★★★ BOTH SETS TRAVEL, and that is the point. `ranges` stays the HARDWARE's coverage
            //     and `allowed` carries the owner's limit, so the client can say WHICH wall a
            //     listener has hit — "the operator does not allow this" and "this radio cannot
            //     hear it" are completely different messages to receive, and telling somebody
            //     their radio is broken when in fact it is policy is the worse of the two
            //     mistakes (Stuart, 2026-08-08).
            if (!perm.empty()) {
                const std::string want = ",\"allowed\":" + vibebands::toJson(perm);
                const size_t close = caps.rfind('}');
                if (close != std::string::npos) caps.insert(close, want);
            }
            j += caps;
        }
        // ★★ IS THERE AN ADMIN PASSWORD, AND ARE WE THROUGH IT? Advertised for the same reason
        // as everything else here: the server ENFORCES the lock (bias-T, PPM, direct sampling
        // and calibration all go through adminGate), but a client that is not told simply draws
        // the controls as normal and the user finds out only when one silently does nothing
        // (Stuart, 2026-07-27: "all controls for the SDR still present"). Enforcement without
        // advertisement is a protection nobody can see.
        { bool aset;
          { std::lock_guard<std::mutex> al(g_vsAdminMtx); aset = !g_vsAdminSecret.empty(); }
          j += std::string(",\"adminSet\":") + (aset ? "true" : "false");
          j += std::string(",\"adminOk\":")  + (adminNow(sock) ? "true" : "false"); }
        // ★★ THE COUNTDOWN NEEDS A DEADLINE AT CONNECT, not just the two warnings. The first
        // cut drove the client's timer ENTIRELY from session_warning at T-120 and T-30 — so on
        // a 30-minute limit the listener saw nothing at all for 28 minutes and concluded the
        // limit had not taken (Stuart, 2026-07-27, connected from his Mac). The warnings are
        // the nudge; this is the clock.
        // -1 = no limit, or this listener is exempt (loopback / admin).
        { const int left = LocalSdrShim::instance().occupantSecsLeft(adminNow(sock) ? 1 : 0);
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
          // ★★★ A LOCKED CENTRE LOCKS THE RATE. The two together ARE the captured window, so
          //     pinning one and leaving the other adjustable is incoherent — and it showed: with
          //     the centre held at 6.5 MHz, changing the rate put the whole display out of
          //     alignment (Stuart, 2026-08-02: "I broke it, you left all the controls
          //     accessible"). Reporting it here is the whole client-side fix: the rate picker
          //     already hides itself and says who set it whenever this is non-zero.
          if (g_vsLockedCentre.load() > 0.0 && sampleRate > 0) lr = sampleRate;
          j += ",\"lockedRate\":" + std::to_string((long long)(lr > 0 ? lr : 0)); }
        // ★★ THE LOCKED WINDOW, published for the same reason lockedRate is: a client that cannot
        //    see the lock offers controls whose every use is a no-op, and the user concludes the
        //    RADIO is broken rather than the control. On a locked receiver the listener gets a
        //    view and a VFO inside the window and no hardware control at all.
        { const double lc = g_vsLockedCentre.load();
          j += ",\"lockedCentre\":" + std::to_string((long long)(lc > 0 ? lc : 0)); }
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
        // ★★ REPORTED, NOT JUST ACCEPTED. A sticky control that does not say its state cannot be
        //    restored by a client that reconnects, and the button then lies about the radio — the
        //    lesson from the July fix that did `nr` and `notch` and left four siblings behind.
        // ★★★ THIS LISTENER'S OWN FOUR, NOT THE SERVER'S. They are per-listener now, so reporting
        //     the shared defaults here would draw somebody else's switches on this person's
        //     screen — and on a shared RSP, where several people really do listen at once, that is
        //     precisely the confusion the change was made to end. Falls back to the shared values
        //     when this receiver has no per-client DSP, where they ARE this listener's.
        const auto myDsp = dspFor(sock);
        const auto mine = [&](std::atomic<bool> ClientDsp::*slot, std::atomic<bool>& shared) {
            return myDsp ? ((*myDsp).*slot).load() : shared.load();
        };
        j += std::string(",\"wsp\":") + (mine(&ClientDsp::wspOn, weakProcOn) ? "true" : "false");
        j += std::string(",\"ims\":") + (mine(&ClientDsp::imsOn, imsOn)      ? "true" : "false");
        j += std::string(",\"ceq\":") + (mine(&ClientDsp::ceqOn, ceqOn)      ? "true" : "false");
        j += std::string(",\"nb\":")  + (mine(&ClientDsp::nbOn,  nbOn)       ? "true" : "false");
        j += std::string(",\"notch\":") + (notchOn.load() ? "true" : "false");
        // ★★★ THE SAME BUG AS `nr`/`notch` ABOVE, AND THE FIX WAS LEFT HALF-DONE. That pair was
        //     added on 2026-07-28 because rendering our saved prefs showed NR OFF while it was
        //     audibly ON. Every OTHER sticky control has exactly the same shape and none of them
        //     were sent — so the fix cured the two controls that had been reported and left its
        //     four siblings to be reported separately later, which is what happened (Stuart,
        //     2026-08-10: auto notch left on for the Airspy still reading OFF on the RSP; "same
        //     with the RF/DAB notches and any NR figure set with the slider too").
        //     ★★ WHEN YOU FIX per-control-state-not-reported, FIX EVERY CONTROL OF THAT SHAPE.
        //        The identical lesson as the `fftRate`/`clientBins` twins — a warning written
        //        next to one field while its neighbour is left alone.
        // ★★ THE STRENGTH, not just the switch. `nr:true` alone cannot restore the SLIDER, so
        //    the client fell back to its own saved number and drew a figure the radio was not
        //    using. A boolean cannot describe a continuous control.
        //    ★ <0 means the listener never set one — send nothing rather than invent a value.
        if (vsDesiredNrStrength() >= 0.0f)
            j += ",\"nrStrength\":" + std::to_string(vsDesiredNrStrength());
        // ★★ Tri-state on the wire as well as in the store: these are only meaningful once
        //    somebody has chosen, and the RSP ones only exist on an RSP at all. Omitting the
        //    key says "no opinion", which is the truth and is what the client must not
        //    overwrite the hardware with.
        if (vsDesiredRfNotch()  >= 0)
            j += std::string(",\"rfNotch\":")  + (vsDesiredRfNotch()  ? "true" : "false");
        if (vsDesiredDabNotch() >= 0)
            j += std::string(",\"dabNotch\":") + (vsDesiredDabNotch() ? "true" : "false");
        // ★★★ TWO DIFFERENT BIAS-TEES, AND THEY ARE NOT INTERCHANGEABLE. `g_biasTeeOn` is the
        //     DONGLE's (rtlsdr_set_bias_tee); the RSP's is a separate setter on a separate
        //     radio, reported under its own key so the client drives the button that exists.
        //     Reporting one as the other would have switched the wrong control on the wrong
        //     hardware — the "else means dongle" shape again.
        j += std::string(",\"biasT\":") + (g_biasTeeOn.load() ? "true" : "false");
        if (vsDesiredRspBiasT() >= 0)
            j += std::string(",\"rspBiasT\":") + (vsDesiredRspBiasT() ? "true" : "false");
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
            connThreads.emplace_back([this, sock]{ routeOrHandle(sock); });
        }
    }

    /** ★★★ IS THIS CONNECTION OURS, OR ANOTHER RADIO'S?
     *
     *  With several radios on one machine the owner forwards ONE port, so the process holding it
     *  answers for all of them: a request for `/r/<serial>/…` belongs to that radio's process, and
     *  the whole connection is handed over (see fd_passing.h). After that we are out of the way
     *  entirely — no proxying, no copying, no shared writer to become a bottleneck.
     *
     *  ★★ THE PEEK HAPPENS HERE, ON THE CONNECTION THREAD, NOT IN accept(). A client that opens a
     *     socket and sends nothing would otherwise stall every other connection behind it, which is
     *     a trivial denial of service and an easy accident on a flaky link.
     *  ★ MSG_PEEK, so the request is left untouched: the receiving process reads an ordinary
     *    request from byte zero rather than us having to replay what we consumed.
     */
    void routeOrHandle(const std::shared_ptr<net::Socket>& sock) {
        HandoffFn router;
        { std::lock_guard<std::mutex> lk(g_vsConfigMtx); router = g_vsHandoffFn; }
        if (router && sock) {
            char buf[1024];
            pollfd pfd{ sock->rawFd(), POLLIN, 0 };
            if (::poll(&pfd, 1, 4000) > 0) {
                const ssize_t n = ::recv(sock->rawFd(), buf, sizeof buf - 1, MSG_PEEK);
                if (n > 0) {
                    buf[n] = 0;
                    const std::string head(buf, (size_t)n);
                    const size_t sp = head.find(' ');
                    const size_t sp2 = sp == std::string::npos ? std::string::npos
                                                              : head.find(' ', sp + 1);
                    if (sp != std::string::npos && sp2 != std::string::npos) {
                        const std::string path = head.substr(sp + 1, sp2 - sp - 1);
                        const std::string dest = router(path);
                        // ★ Routing decisions are invisible when they go wrong: the listener just
                        //   gets an answer from the wrong process, or none. Say what was decided.
                        if (path.rfind("/r/", 0) == 0)
                            LOGI("route %s -> %s", path.c_str(),
                                 dest.empty() ? "(here)" : dest.c_str());
                        if (!dest.empty()) {
                            std::string err;
                            LOGI("handing fd %d to %s", sock->rawFd(), dest.c_str());
                            if (vibe::sendFdTo(dest, sock->rawFd(), err)) {
                                // ★★★ RELEASE, DO NOT CLOSE. close() calls shutdown(), which acts
                                //     on the SOCKET rather than on our descriptor — so it tore down
                                //     the live connection the other process had just adopted, and
                                //     the listener got an empty reply while both sides logged
                                //     success (2026-08-08). We must still drop our reference, or
                                //     the connection never ends when they are finished with it.
                                sock->releaseFd();
                                return;
                            }
                            // ★★ THE RADIO IS NOT THERE — a normal outcome, not an error: its
                            //    process may be starting, stopped, or crashed. Answering it
                            //    ourselves gives the listener a real page instead of a hang.
                            LOGI("handoff to %s failed (%s) — answering here instead",
                                 dest.c_str(), err.c_str());
                        }
                    }
                }
            }
        }
        handleConnection(sock);
    }

    /** Adopt connections handed to us by the process holding the public port. */
    void handoffLoop() {
        vibeThreadName("vibe-handoff");
        LOGI("ready to accept handed-over connections");
        while (serverRunning.load()) {
            const int fd = vibe::fdAccept(handoffFd, 500);
            if (fd < 0) continue;
            LOGI("adopted a handed-over connection");
            auto sock = std::make_shared<net::Socket>(fd);
            std::lock_guard<std::mutex> lk(connMtx);
            connThreads.emplace_back([this, sock]{ handleConnection(sock); });
        }
    }

    // Returns true if this WS may upgrade: no PIN set, or a valid single-use
    // HMAC token in the query string. On failure sends 401, records backoff.
    /** ★★★ THE ADMIN CREDENTIAL, for HTTP endpoints that CHANGE something.
     *
     *  ★★ vsAuthOk() below is the wrong gate for a write, and the difference is the whole point:
     *  the PIN is ACCESS (may you listen at all) and the admin password is CONTROL (may you
     *  change this receiver). They are independent on purpose — the configuration a PUBLIC
     *  receiver wants is NO PIN and an admin password, and in exactly that configuration
     *  vsAuthOk returns true for EVERYONE ("if (secret.empty()) return true").
     *
     *  ★ Same nonce + HMAC challenge as the config and admin APIs. No fourth mechanism.
     *  ★ Loopback and "no admin password set" both pass, matching adminGate(): a server with
     *    nothing to protect must not start refusing its owner. */
    bool vsAdminHttpOk(const std::shared_ptr<net::Socket>& sock, const std::string& reqLine) {
        std::string secret;
        { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
        if (secret.empty()) return true;                 // nothing is protected on this server
        const std::string ip = sock->peerAddress();
        if (isLoopback(ip)) return true;                 // the host IS the operator
        const VsAdminProof pr = vsAdminProof(secret, reqLine);
        if (pr.ok && !g_vsAuthState.blocked(ip)) {
            g_vsAuthState.recordOk(ip);
            return true;
        }
        // ★ Only a WRONG guess counts toward the backoff — a request carrying no credential at
        //   all is not an attempt, and counting it lets a scanner lock the owner out.
        if (pr.guessable) g_vsAuthState.recordFail(ip);
        sock->sendstr("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
        return false;
    }

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
        std::string reqLine, line, wsKey, userAgent, xffHeader, xRealIpHeader;
        long long contentLength = 0;      // ★ needed by POST /vibeserver/config; 0 for everything else
        if (sock->recvline(reqLine, 8192, 5000) <= 0) { sock->close(); return; }
        // ★★★ STRIP OUR OWN /r/<serial> PREFIX, ONCE, RIGHT HERE.
        //
        //     When several radios share one forwarded port, the front door routes on a path prefix
        //     and hands the connection over untouched — so what arrives here is
        //     `GET /r/240513CA60/ws/audio`. Every route below matches on the bare path, and there
        //     are dozens of them: stripping at each would be a list to keep in step for ever, and
        //     the first one forgotten is a 404 that only appears on multi-radio machines.
        // ★★ ONLY OUR OWN. A prefix naming a DIFFERENT radio must not be quietly served by us —
        //    that would answer for a receiver we are not, with our frequency and our listeners.
        // ★★ A FRONT DOOR CANNOT ANSWER RADIO QUESTIONS, so it says so rather than reaching for a
        //    device it does not have. Everything a listener needs BEFORE choosing a radio — the
        //    landing page, the radio list, setup, admin — is served; anything that needs IQ is a
        //    503 with a reason, which is what an honest "ask a radio, not me" looks like.
        if (frontDoorOnly) {
            const size_t sp0 = reqLine.find(' ');
            const std::string path0 = sp0 == std::string::npos ? "/" :
                reqLine.substr(sp0 + 1, reqLine.find(' ', sp0 + 1) - sp0 - 1);
            const bool ok =
                   path0 == "/" || path0.rfind("/?", 0) == 0
                || path0.rfind("/setup", 0) == 0
                || path0.rfind("/vibeserver.json", 0) == 0
                || path0.rfind("/vibeserver/radios", 0) == 0
                || path0.rfind("/vibeserver/stationlogo", 0) == 0
                || path0.rfind("/vibeserver/auth", 0) == 0
                || path0.rfind("/vibeserver/config", 0) == 0
                || path0.rfind("/vibeserver/admin", 0) == 0
                || path0.rfind("/vibeserver/conditions", 0) == 0
                // ★ The shortwave schedule is a MACHINE-wide file (/var/lib/vibeserver/eibi.csv),
                //   shared by every radio process, and the setup page that asks for it is served
                //   by this door. Refusing it here made the page report "could not reach the
                //   server" about a server it was talking to (Stuart, 2026-08-08).
                || path0.rfind("/vibeserver/eibi", 0) == 0
                || path0.rfind("/vibeserver/rtl-serial", 0) == 0
                || path0.rfind("/vibeserver/spectrogram", 0) == 0
                // ★★★ THE SETUP PAGE'S SERVER TAB NEEDS THIS DOOR TO ANSWER. A 503 here made the
                //   whole Server tab unusable — "cant edit anything, doesnt save, reports Could
                //   not reach server" (Saber, 2026-08-09) — because the tab reads the CPU
                //   governor and clock from this endpoint and the failed fetch took the rest of
                //   the render with it. The door has no RADIO, but it does have a MACHINE, and
                //   everything the Server tab asks about belongs to the machine.
                // ★ It answers honestly below: driver "none", present false, no rates, no gains.
                || path0.rfind("/vibeserver/hardware", 0) == 0
                || path0.rfind("/bookmarks", 0) == 0
                || path0.rfind("/favicon", 0) == 0
                // ★ The page's own furniture. A 503 for the web manifest put a red error in every
                //   listener's console on a page that was working perfectly — noise that makes the
                //   real errors beside it easy to miss.
                || path0.rfind("/manifest", 0) == 0
                || path0.rfind("/icon", 0) == 0
                || path0.rfind("/apple-touch-icon", 0) == 0;
            if (!ok) {
                // ★★★ A DEAD END IS NOT AN ANSWER. This used to reply with a bare JSON error, so a
                //     listener who simply refreshed their tab — the first thing anyone tries when
                //     something looks stuck — got a wall of text with no way back and no idea what
                //     had happened (Stuart, 2026-08-08).
                //
                // ★★ IT REACHES A PERSON, SO IT LOOKS LIKE ONE. A path under /r/ that we could not
                //    route means that radio's process is not answering right now; anything else
                //    means they have asked the front door for something only a radio has. Either
                //    way: say so in a sentence, and give them the door back.
                // ★ 503 with Retry-After, not a redirect. A refresh should land them where they
                //   were once the radio is back, and bouncing them elsewhere would lose the very
                //   thing they were trying to keep.
                const bool wasRadio = path0.rfind("/r/", 0) == 0;
                const std::string body = std::string(
                    "<!doctype html><meta charset=utf-8>"
                    "<title>VibeServer</title>"
                    "<style>body{background:#0b0b0b;color:#ffb000;font:14px/1.6 ui-monospace,monospace;"
                    "display:flex;min-height:100vh;align-items:center;justify-content:center;margin:0}"
                    "div{max-width:32em;padding:2em}a{color:#ffb000}</style><div>"
                    "<h1 style='font-size:1.2em;letter-spacing:.1em'>VIBESERVER</h1><p>")
                    + (wasRadio
                        ? "That radio is not answering at the moment. It may be starting up, or it "
                          "may have been stopped."
                        : "This is the front door. It lists the radios on this machine; the "
                          "receivers themselves are behind it.")
                    + "</p><p><a href=\"/\">See the radios on this machine</a></p>"
                      "<p style='opacity:.6'>Refreshing this page is safe — it will work again as "
                      "soon as the radio is back.</p></div>";
                sock->sendstr("HTTP/1.1 503 Service Unavailable\r\n"
                              "Content-Type: text/html; charset=utf-8\r\n"
                              "Retry-After: 5\r\nConnection: close\r\n"
                              "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
                sock->close(); return;
            }
        }
        // ★★★ EITHER NAME. The front door routes /r/<id>/ and /r/<serial>/ to the same socket, so
        //     a radio that strips only one of them 404s the other — which is exactly what happened
        //     when links moved to the opaque id: the hand-off succeeded and the radio then refused
        //     the path it had just been handed (2026-08-09).
        {
            std::lock_guard<std::mutex> lk(g_vsConfigMtx);
            const size_t sp = reqLine.find(' ');
            for (const std::string* pre : { &g_vsPathPrefix, &g_vsPathPrefixAlt }) {
                if (pre->empty() || sp == std::string::npos) continue;
                if (reqLine.compare(sp + 1, pre->size(), *pre) != 0) continue;
                // Leave a leading '/' behind: "/r/ABC" + "/ws/audio" -> "/ws/audio", and
                // "/r/ABC" alone -> "/", which is the receiver page.
                const size_t cut = sp + 1 + pre->size();
                const bool bare = (cut >= reqLine.size() || reqLine[cut] == ' ');
                reqLine = reqLine.substr(0, sp + 1) + (bare ? "/" : "") + reqLine.substr(cut);
                break;
            }
        }
        while (sock->recvline(line, 8192, 5000) > 0) {
            if (line.empty() || line == "\r") break;
            if (line.size() > 15) {
                std::string cl = line.substr(0, 15);
                for (auto& c : cl) c = (char)tolower(c);
                if (cl == "content-length:") contentLength = atoll(line.c_str() + 15);
            }
            if (line.size() > 11) {
                std::string ua = line.substr(0, 11);
                for (auto& c : ua) c = (char)tolower(c);
                if (ua == "user-agent:") {
                    auto vv = line.substr(11);
                    size_t a = vv.find_first_not_of(" \t");
                    size_t b = vv.find_last_not_of(" \t\r\n");
                    if (a != std::string::npos) userAgent = vv.substr(a, b - a + 1);
                }
            }
            // ★ Who the client really is, when the owner has said this peer may tell us.
            //   Captured here with the others; the decision is made once the peer is known.
            if (line.size() > 16) {
                std::string fk = line.substr(0, 16);
                for (auto& c : fk) c = (char)tolower(c);
                if (fk == "x-forwarded-for:") {
                    auto vv = line.substr(16);
                    size_t a = vv.find_first_not_of(" \t");
                    size_t b = vv.find_last_not_of(" \t\r\n");
                    if (a != std::string::npos) xffHeader = vv.substr(a, b - a + 1);
                }
            }
            if (line.size() > 10) {
                std::string rk = line.substr(0, 10);
                for (auto& c : rk) c = (char)tolower(c);
                if (rk == "x-real-ip:") {
                    auto vv = line.substr(10);
                    size_t a = vv.find_first_not_of(" \t");
                    size_t b = vv.find_last_not_of(" \t\r\n");
                    if (a != std::string::npos) xRealIpHeader = vv.substr(a, b - a + 1);
                }
            }
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
        // ★★★ RESOLVE WHO THIS ACTUALLY IS, BEFORE ANYTHING READS THE ADDRESS. Bans, geo/ASN,
        //     the connection log and the admin lockout all call peerAddress(); doing the
        //     substitution once here means none of them change, and none of them can be missed.
        //     ★ socketPeerAddress() deliberately, NOT peerAddress(): the trust decision must be
        //       made on the real TCP peer, or a proxy could nominate its own successor.
        if (!xffHeader.empty() || !xRealIpHeader.empty()) {
            std::string real;
            {
                std::lock_guard<std::mutex> lk(g_vsTrustedProxiesMtx);
                real = vibeproxy::clientAddress(g_vsTrustedProxies, sock->socketPeerAddress(),
                                                xffHeader, xRealIpHeader);
            }
            if (real != sock->socketPeerAddress()) sock->setEffectiveAddress(real);
        }

        bool wsSpec  = reqLine.find("/ws/user-spectrum") != std::string::npos;
        bool wsAudio = reqLine.find("/ws/audio") != std::string::npos;
        bool wsDx    = reqLine.find("/ws/dxcluster") != std::string::npos;

        // VibeServer PIN pre-flight: the client fetches a nonce here, computes
        // HMAC(pin, nonce), then opens the WS with ?vs_nonce=&vs_auth=. When no
        // PIN is set we say so (required:false) and everything behaves as UberSDR.
        // ★★★ MINT A TICKET THAT EVERY RADIO WILL ACCEPT. The owner authenticates ONCE, here
        //     (usually at the front door, which owns no radio), and then walks into any radio on
        //     the machine as admin. Without this the landing page's admin box could not work at
        //     all: each radio is its own process with its own nonce store, so a credential proved
        //     here is meaningless there (Stuart, 2026-08-08: "Server refused the connection").
        // ★★ Getting a ticket requires being admin ALREADY — adminOkFor() applies the same
        //    handshake, lockout and empty-secret rules as every other admin route. This mints a
        //    lease on that proof; it never creates it.
        if (reqLine.find("/vibeserver/admin-ticket") != std::string::npos) {
            std::string secret;
            { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
            if (secret.empty()) {
                sock->sendstr("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                sock->close(); return;
            }
            if (!adminOkFor(reqLine, sock)) {
                sock->sendstr("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                LOGI("admin ticket refused for %s", sock->peerAddress().c_str());
                sock->close(); return;
            }
            const std::string ticket = vsMintAdminTicket();
            const std::string body = "{\"ticket\":\"" + ticket + "\",\"ttl\":"
                                   + std::to_string(vibeadmin::kTicketTtlSec) + "}";
            // ★ no-store: a cached admin ticket in a shared proxy would outlive the tab it was
            //   minted for, which is the one thing a short lease is meant to prevent.
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            LOGI("admin ticket issued to %s", sock->peerAddress().c_str());
            sock->close(); return;
        }

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
            acceptDxcluster(sock, wsKey, queryParam(reqLine, "user_session_id"));
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
                wantBins = bq.empty() ? WIRE_BINS_DEFAULT : atoi(bq.c_str());
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
                     queryParam(reqLine, "vs_admin_auth"),
                     queryParam(reqLine, "vs_admin_ticket"),
                     // ★★★ A CLIENT THAT CANNOT SET A HEADER MAY NAME ITSELF IN THE QUERY. React
                     //     Native's WebSocket does not reliably send a User-Agent on the upgrade —
                     //     it is one of the headers the platform owns — so every VibeSDR session
                     //     landed in the owner's log as "—", beside browsers naming themselves in
                     //     full (Stuart, 2026-08-13, still true after the header was added). The
                     //     header is still preferred where it exists; this is the fallback, and it
                     //     is the ONLY thing a WebSocket client can always control.
                     // ★★ Trusted no further than a User-Agent already is: both are the client's
                     //    own word about itself, neither grants anything, and the log says who
                     //    CLAIMED to be there — which is all a User-Agent has ever meant.
                     userAgent.empty() ? queryParam(reqLine, "client") : userAgent,
                     // ★ Whether this arrival is ALLOWED to displace the current listener — see
                     //   the override decision in acceptWs. Absent means yes, so nothing that
                     //   predates the flag changes behaviour.
                     queryParam(reqLine, "vs_takeover") != "0");
        // ★★ MATCHED ON A PATH, NOT A SUBSTRING ANYWHERE IN THE REQUEST LINE. This was
        //    `reqLine.find("/connection") != npos`, which quietly claimed every later route whose
        //    path merely CONTAINS that word — /vibeserver/admin/connections was answered by the
        //    preflight, so the admin connection log returned {"allowed":true} and looked, from
        //    outside, like an unauthenticated endpoint leaking. A substring match against a whole
        //    request line is a route that grows new meanings every time somebody adds a URL.
        } else if (reqLine.find("/connection ") != std::string::npos ||
                   reqLine.find("/connection?") != std::string::npos) {
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
            // ★★★ AN ADMIN IS NEVER "IN USE" — THEY ARE THE ONE PERSON WHO CAN TAKE IT. This
            //     preflight knew nothing about admin, so an owner holding the password was told
            //     "Connection check failed: in-use" and STOPPED THERE, before a socket was ever
            //     opened. Everything downstream — the ticket, the eviction, the whole handshake
            //     path — was correct and never got the chance to run (Stuart, 2026-08-15, after
            //     three client fixes at layers below this one: "entered the admin password at the
            //     bottom which unlocked the radio, tried to use it, and this is what I am met
            //     with").
            // ★★ A GATE THAT DOES NOT KNOW THE EXCEPTION IS A GATE THAT ENFORCES THE OPPOSITE
            //    RULE. The WS handshake has always understood that admin outranks occupancy; this
            //    check was written to spare a client the trouble of opening sockets it could not
            //    use, and by omitting the exception it refused the one client that could.
            // ★ Same credential as everywhere else, checked with the same helper, so the lockout
            //   and the empty-secret rule cannot drift apart from the handshake's copy.
            if (busy && adminOkFor(reqLine, sock)) busy = false;
            // ★ The preflight exists so a client learns it cannot connect BEFORE opening
            //   sockets. A ban is the most certain "no" we have, so leaving it out would mean
            //   the one refusal that never changes is also the one the client has to discover
            //   the hard way.
            const bool isBanned = !isLoopback(sock->peerAddress())
                               && LocalSdrShim::isBanned(sock->peerAddress());
            std::string body = isBanned
                ? "{\"allowed\":false,\"reason\":\"banned\"}"
                : busy
                ? "{\"allowed\":false,\"reason\":\"in-use\"}"
                : "{\"allowed\":true}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        // ── ★★★ THE CONFIG API ────────────────────────────────────────────────────────────
        // GET  /vibeserver/config  -> the stored settings, as JSON
        // POST /vibeserver/config  -> replace them, persist, and report whether a restart is due
        //
        // Both are gated on the ADMIN credential using the SAME nonce + HMAC challenge-response
        // the admin unlock and the admin override already use — no new credential mechanism, and
        // the password never crosses the wire.
        // ★★ The browser setup page is a CLIENT of this. So is the VibeSDR app, later, over a
        //    captive portal on a Pi up a tree. That is the whole reason it is an endpoint and not
        //    form handling wired into this router — see local_sdr_shim.h.
        // ── ★★ WHAT RADIO IS THIS? — read-only, for the setup page's hardware panel ──────
        // Separate from /vibeserver/config on purpose: config is what you SET, this is what you
        // HAVE. Mixing them would invite a page to POST back a field the server derives.
        // ★★★ THE SETUP PAGE MUST BRANCH ON THIS. AGENTS.md: "a control that only works on one
        //     radio should not be there." The three supported radios do not share a gain model —
        //     RTL has a gain LIST, the Airspy HF+ has no variable gain at all (attenuator +
        //     preamp), the SDRplay RSP uses IF gain REDUCTION. Drawing one set of sliders for all
        //     three leaves two of them inert, and a control that does nothing reads as a broken
        //     FEATURE, not a wrong control.
        // ── ★★ THE SPECTROGRAM, for the landing page ─────────────────────────────────────
        // Binary, because 1440 rows x 512 bins as JSON numbers would be megabytes of text for a
        // picture. Header carries what the STAMPS need: the span (for the frequency scale down
        // the top) and each row's wall-clock time (for the time scale down the side).
        //   magic "VSPG" | u8 ver | u16 bins | u16 rows | f64 centreHz | f64 spanHz
        //   then per row: i64 epoch-ms, then `bins` bytes of dB
        // ★ Deliberately NOT admin-gated: it is the public face of the receiver, and it shows
        //   nothing a listener could not see by watching the waterfall for a day.
        } else if (reqLine.rfind("GET /vibeserver/spectrogram", 0) == 0) {
            vsNoteVisitor(sock->peerAddress());   // the page refreshes this while it is open
            // ★★ The caller says how big its canvas is; we downsample to fit. Sending the full
            //    2048 x 1440 (~3 MB) to draw on a 900-pixel-wide splash would be paying for detail
            //    the screen cannot show — and on a landing page, load time IS the feature.
            //    ★ Peak-hold when reducing, never averaging: a narrow carrier must survive, which
            //      is the only reason anyone looks at one of these.
            int wantBins = atoi(queryParam(reqLine, "bins").c_str());
            int wantRows = atoi(queryParam(reqLine, "rows").c_str());
            if (wantBins <= 0 || wantBins > kSpectroBins) wantBins = kSpectroBins;
            if (wantRows <= 0) wantRows = kSpectroRows;
            std::vector<uint8_t> out;
            {
                std::lock_guard<std::mutex> lk(spectroMtx);
                const uint16_t nb = (uint16_t)wantBins;
                const uint16_t nr = (uint16_t)std::min<size_t>(spectro.size(), (size_t)wantRows);
                const double centre = g_vsLockedCentre.load() > 0 ? g_vsLockedCentre.load()
                                                                  : rtlCenter.load();
                const double span = displaySpan();
                out.reserve(16 + (size_t)nr * (8 + nb));
                auto put = [&](const void* p, size_t n) {
                    const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); };
                out.push_back('V'); out.push_back('S'); out.push_back('P'); out.push_back('G');
                out.push_back(1);
                put(&nb, 2); put(&nr, 2);
                put(&centre, 8); put(&span, 8);
                // ★ Newest rows win when there are more than asked for: a landing page showing
                //   the last few hours in detail beats one showing a day as mush.
                const size_t skip = spectro.size() - (size_t)nr;
                size_t idx = 0;
                for (auto& r : spectro) {
                    if (idx++ < skip) continue;
                    put(&r.atMs, 8);
                    if (nb == kSpectroBins) { put(r.bins.data(), r.bins.size()); continue; }
                    std::vector<uint8_t> row((size_t)nb);
                    for (int i = 0; i < nb; i++) {
                        const int lo = (int)((int64_t)i * kSpectroBins / nb);
                        const int hi = (int)((int64_t)(i + 1) * kSpectroBins / nb);
                        uint8_t best = 0;
                        for (int j = lo; j < hi && j < kSpectroBins; j++)
                            if (r.bins[j] > best) best = r.bins[j];
                        row[i] = best;
                    }
                    put(row.data(), row.size());
                }
            }
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                          "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                          "Connection: close\r\nContent-Length: "
                          + std::to_string(out.size()) + "\r\n\r\n");
            if (!out.empty()) sock->send(out.data(), out.size());
            sock->close();
            return;

        } else if (reqLine.rfind("GET /vibeserver/hardware", 0) == 0) {
            // ★★★ A DOOR THAT OWNS NO RADIO MUST NOT DESCRIBE ONE. Without this the front door
            //     answered driver "rtl", present true — the default arm of the chain below — and
            //     the setup page drew dongle controls, a dongle's gain list and a dongle's sample
            //     rates for a machine holding an RSP and an Airspy. Reporting nothing is the
            //     honest answer; reporting a dongle is the same "else means dongle" trap this
            //     tree has fixed twice already.
            const bool noRadio = frontDoorOnly;
            const bool rsp = !noRadio && LocalSdrShim::instance().isSdrplay();
            const bool hf  = !noRadio && LocalSdrShim::instance().isAirspyHf();
            const bool lost = noRadio || deviceLost.load();
            std::string j = std::string("{\"driver\":\"")
                          + (lost ? "none" : rsp ? "sdrplay" : hf ? "airspyhf" : "rtl")
                          + "\",\"present\":" + (lost ? "false" : "true")
                          + ",\"rates\":[" + supportedRates() + "],\"gains\":[";
            // ★★ THE BAND NAMES COME FROM THE SERVER, not from a copy in the page. Only the id and
            //    the label travel: the page never learns an edge, so the two cannot drift into
            //    disagreeing about where "VHF airband" ends — which is the kind of difference
            //    nobody notices until a listener is somewhere the owner thought they had blocked.
            if (!rsp && !hf && !lost) {
                std::vector<int> g = LocalSdrShim::instance().getTunerGains();
                for (size_t i = 0; i < g.size(); i++) { if (i) j += ','; j += std::to_string(g[i]); }
            }
            j += "]";
            // ★★★ HOW MANY RF GAIN POSITIONS THIS RSP HAS. The limiter caps an RF POSITION, and the
            //     count is a property of the MODEL (RSP1 4, RSP1A/1B/duo 10, RSP2 9, RSPdx 28) — so
            //     the setup page could not draw a slider for it while this was published only in
            //     the admin status (Stuart, 2026-08-12: "isn't the RF gain 0-9 on an RSP?" — yes on
            //     his RSP1B, and 0-27 on an RSPdx, which is exactly why it must be READ, not assumed).
            // ★ 0 when it is not an RSP or no radio is attached: the page then shows its text box
            //   rather than a slider with an invented maximum.
            {
                const int n = (rsp && !lost) ? LocalSdrShim::instance().rfGainPositions() : 0;
                j += ",\"lnaStates\":" + std::to_string(n);
            }
            j += ",\"bands\":[";
            {
                const auto& bs = vibebands::namedBands();
                for (size_t i = 0; i < bs.size(); ++i)
                    j += std::string(i ? "," : "") + "{\"id\":\"" + bs[i].id + "\",\"label\":\""
                       + vibeadmin::esc(bs[i].label) + "\"}";
            }
            j += "]";
            // ★★ WHAT THE MACHINE IS ACTUALLY DOING, not what was asked for. A governor is applied
            //    by the service at start and can fail (kernel without it, a read-only sysfs, a
            //    container) — and a settings page that shows the REQUEST rather than the state is
            //    how an owner ends up certain they fixed something they did not. Same rule as the
            //    mDNS name: display what is true.
            {
                std::string gov, mhz;
                if (FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r")) {
                    char b[64] = {0}; if (fgets(b, sizeof b, f)) gov = b; fclose(f);
                    while (!gov.empty() && (gov.back()=='\n'||gov.back()=='\r')) gov.pop_back();
                }
                if (FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r")) {
                    char b[64] = {0}; if (fgets(b, sizeof b, f)) mhz = b; fclose(f);
                    while (!mhz.empty() && (mhz.back()=='\n'||mhz.back()=='\r')) mhz.pop_back();
                }
                if (!gov.empty()) j += ",\"governor\":\"" + jsonEscape(gov) + "\"";
                if (!mhz.empty()) j += ",\"cpuKHz\":" + mhz;
            }
            j += "}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(j.size()) + "\r\n\r\n" + j);
            sock->close();
            return;

        } else if (reqLine.rfind("GET /vibeserver/config", 0) == 0 ||
                   reqLine.rfind("POST /vibeserver/config", 0) == 0) {
            const bool isPost = reqLine.rfind("POST", 0) == 0;
            LocalSdrShim::ConfigGetFn getFn; LocalSdrShim::ConfigSetFn setFn;
            { std::lock_guard<std::mutex> lk(g_vsConfigMtx); getFn = g_vsConfigGet; setFn = g_vsConfigSet; }
            auto reply = [&](int code, const char* status, const std::string& body) {
                sock->sendstr("HTTP/1.1 " + std::to_string(code) + " " + status +
                              "\r\nContent-Type: application/json\r\nCache-Control: no-store"
                              "\r\nConnection: close\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body);
                sock->close();
            };
            if (!getFn) {   // a phone: the endpoints exist in the code but not on this platform
                reply(501, "Not Implemented",
                      "{\"error\":\"this build does not store server configuration\"}");
                return;
            }
            // ★★★ AUTHORISE FIRST, AND REFUSE WHEN NO PASSWORD IS SET. An unconfigured server
            //     still has an admin password — the wizard makes it mandatory — so "no password"
            //     here means something is wrong, and serving the config (which CONTAINS the PIN)
            //     to an unauthenticated caller would be the worst possible default.
            std::string secret;
            { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
            const std::string ip = sock->peerAddress();
            const VsAdminProof pr = vsAdminProof(secret, reqLine);
            const bool authed = pr.ok && !g_vsAuthState.blocked(ip);
            if (!authed) {
                // ★★ Same fix as the admin API's gate: a request with no credentials is not a
                //    failed guess, and counting it lets anyone lock the owner out of their own
                //    settings by hitting this URL a handful of times without ever guessing.
                if (!secret.empty() && pr.guessable) g_vsAuthState.recordFail(ip);
                LOGI("config API refused for %s", ip.c_str());
                reply(401, "Unauthorized", "{\"error\":\"admin password required\"}");
                return;
            }
            g_vsAuthState.recordOk(ip);
            if (!isPost) { reply(200, "OK", getFn()); return; }

            // ── POST: read the body, hand it to the daemon, report the outcome ──────────────
            const long long clen = contentLength;   // captured while reading the headers
            if (clen <= 0 || clen > 256 * 1024) {
                reply(400, "Bad Request", "{\"error\":\"missing or oversized body\"}");
                return;
            }
            std::string body;
            body.resize((size_t)clen);
            size_t got = 0;
            while (got < body.size()) {
                int n = sock->recv((uint8_t*)&body[got], body.size() - got, false, 5000);
                if (n <= 0) break;
                got += (size_t)n;
            }
            if (got != body.size()) {
                reply(400, "Bad Request", "{\"error\":\"short body\"}");
                return;
            }
            std::string err;
            if (!setFn(body, err)) {
                LOGI("config API rejected a save: %s", err.c_str());
                reply(400, "Bad Request", "{\"error\":\"" + jsonEscape(err) + "\"}");
                return;
            }
            LOGI("config saved by %s", ip.c_str());
            // ★ SAY THAT A RESTART IS COMING. Most of these settings are read once at start, so
            //   the honest answer is "saved, and the server is restarting" — a page that said
            //   only "saved" would leave the owner watching settings that have not taken effect.
            reply(200, "OK", "{\"ok\":true,\"restart\":true}");
            return;

        // ── ★★★ THE ADMIN API — /vibeserver/admin/* ───────────────────────────────────────
        // Monitoring, the listener list, the ban list and the four maintenance actions. Reached
        // from the SERVER ADMIN button at the bottom of the client's menu, which appears only
        // once the admin session is unlocked.
        //
        // ★★★ ONE GATE FOR ALL OF IT, and it is the SAME nonce + HMAC challenge the config API,
        //     the admin unlock and the admin override already use. Not a new credential, not a
        //     cookie, not a bearer token: a fourth authentication mechanism is a fourth thing to
        //     get wrong, and this one inherits the brute-force lockout for free.
        //
        // ★★★ THERE IS NO TERMINAL HERE, AND THAT IS A DECISION, NOT AN OMISSION.
        //     UberSDR ships one (`gotty_client.go`: POST /api/exec taking an arbitrary command
        //     string). Stuart wanted the capability — "send a reboot command from my iPhone if
        //     the server was having issues... loft or up the allotment" — and raised the security
        //     worry himself. The argument against is not that a shell is risky in the abstract:
        //       • The admin password's day job is guarding GAIN SLIDERS. It was chosen as a
        //         convenience credential. A terminal behind it is root on the box and the home
        //         network behind it.
        //       • These receivers are PUBLICLY LISTED (instances.ubersdr.org), served over plain
        //         HTTP, and port-forwarded. Bots scan for exactly this shape.
        //       • ★ The decisive point is that a terminal is an UNBOUNDED hole for a BOUNDED
        //         need. The need is four buttons. Buttons are loggable, rate-limitable, and
        //         cannot be repurposed into something nobody has thought of yet.
        //       • It also duplicates SSH with weaker auth. For "a shell from my phone", the right
        //         answer is Tailscale/WireGuard — real key auth, nothing exposed.
        //     ★★ UberSDR's own source is the best argument against its own feature: its server
        //        only ever calls that exec API with four fixed commands (`ip address show`, read
        //        the cpufreq governor, `check_time.sh`, `uptime`). It built a general hole to do
        //        four specific things.
        } else if (reqLine.rfind("GET /vibeserver/admin/", 0) == 0 ||
                   reqLine.rfind("POST /vibeserver/admin/", 0) == 0) {
            const bool isPost = reqLine.rfind("POST", 0) == 0;
            auto reply = [&](int code, const char* status, const std::string& body) {
                sock->sendstr("HTTP/1.1 " + std::to_string(code) + " " + status +
                              "\r\nContent-Type: application/json\r\nCache-Control: no-store"
                              "\r\nConnection: close\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body);
                sock->close();
            };
            // ★★ SAME GATE AS THE CONFIG API, INCLUDING "REFUSE WHEN NO PASSWORD IS SET".
            //    A server with no admin password has no owner we can recognise, so the honest
            //    answer to "show me your listeners' IP addresses" is no, not yes-to-everyone.
            std::string secret;
            { std::lock_guard<std::mutex> lk(g_vsAdminMtx); secret = g_vsAdminSecret; }
            const std::string ip = sock->peerAddress();
            const VsAdminProof pr = vsAdminProof(secret, reqLine);
            const bool authed = pr.ok && !g_vsAuthState.blocked(ip);
            if (!authed) {
                // ★★★ ONLY A WRONG GUESS COUNTS AS A GUESS. Recording a failure for a request
                //     that carried NO credentials at all turns the brute-force defence into a
                //     denial of service against the owner: any scanner hitting /vibeserver/admin/
                //     a few times — and they will, these servers are publicly listed — locks the
                //     legitimate admin out of their own receiver for the backoff period, from an
                //     address the attacker never has to guess anything from.
                //     ★ Found by the probe in tools/vibeserver-probes/admin-api.mjs: its five
                //       deliberate no-credential requests locked out its own next real one.
                if (!secret.empty() && pr.guessable) g_vsAuthState.recordFail(ip);
                LOGI("admin API refused for %s", ip.c_str());
                reply(401, "Unauthorized", "{\"error\":\"admin password required\"}");
                return;
            }
            g_vsAuthState.recordOk(ip);

            // Everything after "/vibeserver/admin/" up to the query or the HTTP version.
            std::string what;
            {
                const size_t a = reqLine.find("/vibeserver/admin/");
                size_t b = reqLine.find_first_of(" ?", a);
                if (b == std::string::npos) b = reqLine.size();
                what = reqLine.substr(a + 18, b - a - 18);
            }

            // ── Read the body for the POSTs. Small by construction: the largest is a ban. ──
            std::string body;
            if (isPost) {
                const long long clen = contentLength;
                if (clen < 0 || clen > 64 * 1024) {
                    reply(400, "Bad Request", "{\"error\":\"oversized body\"}"); return;
                }
                body.resize((size_t)clen);
                size_t got = 0;
                while (got < body.size()) {
                    int n = sock->recv((uint8_t*)&body[got], body.size() - got, false, 5000);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                if (got != body.size()) { reply(400, "Bad Request", "{\"error\":\"short body\"}"); return; }
            }

            if (!isPost && what == "status") {
                reply(200, "OK", LocalSdrShim::instance().adminStatusJson());
                return;
            }
            if (!isPost && what == "sessions") {
                reply(200, "OK", LocalSdrShim::instance().adminSessionsJson());
                return;
            }
            if (!isPost && what == "maintenance-log") {
                // ★ Polled once a second while an action runs, so it must be cheap and must never
                //   block: the handler reads a file and returns. No systemd, no subprocess.
                LocalSdrShim::AdminLogFn fn;
                { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsAdminLogFn; }
                if (!fn) { reply(200, "OK", "{\"running\":false,\"text\":\"\"}"); return; }
                std::string text; bool running = false; int code = 0;
                fn(text, running, code);
                reply(200, "OK", std::string("{\"running\":") + (running ? "true" : "false")
                               + ",\"exitCode\":" + std::to_string(code)
                               + ",\"text\":\"" + vibeadmin::esc(text) + "\"}");
                return;
            }
            if (!isPost && what == "history") {
                reply(200, "OK", g_vsHistory.json());
                return;
            }
            if (!isPost && what == "connections") {
                reply(200, "OK", "{\"connections\":" + g_vsConnLog.json() + "}");
                return;
            }
            if (!isPost && what == "notice") {
                // ★ secondsLeft: -1 = no end date, 0 = nothing showing, >0 = seconds. Three
                //   distinct answers, because "until you clear it" must not read as "about to go".
                reply(200, "OK", "{\"text\":\"" + vibeadmin::esc(g_vsNotice.current())
                                 + "\",\"secondsLeft\":" + std::to_string(g_vsNotice.secondsLeft()) + "}");
                return;
            }
            if (!isPost && what == "bans") {
                reply(200, "OK", "{\"bans\":" + g_vsBans.json() + "}");
                return;
            }
            if (isPost && what == "ban") {
                const std::string cidr   = jsonStr(body, "cidr");
                const std::string reason = jsonStr(body, "reason");
                double minsV = 0;
                const int mins = jsonNum(body, "minutes", minsV) ? (int)minsV : 0;
                std::string err;
                if (cidr.empty()) { reply(400, "Bad Request", "{\"error\":\"no address given\"}"); return; }
                if (!g_vsBans.add(cidr, reason, mins, err)) {
                    reply(400, "Bad Request", "{\"error\":\"" + jsonEscape(err) + "\"}"); return;
                }
                // ★★ A BAN THAT LEAVES THE BANNED PERSON CONNECTED IS NOT A BAN. It only takes
                //    effect on the NEXT connection otherwise, and the owner — watching the
                //    listener they just banned carry on listening — reasonably concludes it did
                //    not work. Kick every session the new rule now matches.
                const int kicked = LocalSdrShim::instance().adminKickMatching(cidr);
                LOGI("admin banned %s (%s) — kicked %d", cidr.c_str(), reason.c_str(), kicked);
                reply(200, "OK", "{\"ok\":true,\"kicked\":" + std::to_string(kicked) + "}");
                return;
            }
            // ★★ THE NOTICE. Its own endpoint rather than a config field, because it is the one
            //    setting an owner reaches for while something is ACTIVELY wrong — halfway up a
            //    ladder, on a phone — and it must take effect at once on every radio without a
            //    restart or a config round-trip.
            if (isPost && what == "notice") {
                const std::string text = jsonStr(body, "text");
                double minsV = 0;
                const int mins = jsonNum(body, "minutes", minsV) ? (int)minsV : 0;
                // ★ A limit, not a validation error: a notice is a sentence, and anything longer
                //   would be pushed to every client on every identity fetch.
                std::string err;
                if (!LocalSdrShim::setNotice(text.substr(0, 240), mins, err)) {
                    reply(400, "Bad Request", "{\"error\":\"" + jsonEscape(err) + "\"}"); return;
                }
                LOGI("admin notice %s (%d min)", text.empty() ? "cleared" : "posted", mins);
                // ★★★ TELL THE PEOPLE ALREADY LISTENING. They are exactly who it is for — a
                //     notice that only reaches the NEXT visitor misses everyone currently watching
                //     the spectrum misbehave and drawing their own conclusions.
                LocalSdrShim::instance().broadcastNotice();
                reply(200, "OK", "{\"ok\":true}");
                return;
            }
            if (isPost && what == "unban") {
                const std::string cidr = jsonStr(body, "cidr");
                const bool ok = g_vsBans.remove(cidr);
                reply(ok ? 200 : 404, ok ? "OK" : "Not Found",
                      ok ? "{\"ok\":true}" : "{\"error\":\"no such ban\"}");
                return;
            }
            // ★★★ THREE SEPARATE CLEARS, DELIBERATELY. What they empty differs in kind: the
            //     learned list is a decaying GUESS, the manual list is what somebody typed, and the
            //     logo cache is somebody else's answer we chose to remember. One button for all
            //     three would make an owner destroy two things they trust to fix the one they do
            //     not.
            if (isPost && what == "clear") {
                const std::string kind = queryParam(reqLine, "what");
                if (kind == "learned")      { bmClearLearned(); }
                else if (kind == "manual")  { bmClearManual(); }
                else if (kind == "logos")   { LocalSdrShim::clearLogoCache(); }
                else { reply(400, "Bad Request", "{\"error\":\"what must be learned|manual|logos\"}"); return; }
                LOGI("admin cleared: %s", kind.c_str());
                reply(200, "OK", "{\"ok\":true}");
                return;
            }
            if (isPost && what == "kick") {
                const std::string session = jsonStr(body, "session");
                const std::string addr    = jsonStr(body, "ip");
                const int n = LocalSdrShim::instance().adminKick(session, addr);
                LOGI("admin kicked %s%s — %d session(s)", session.c_str(), addr.c_str(), n);
                reply(200, "OK", "{\"ok\":true,\"kicked\":" + std::to_string(n) + "}");
                return;
            }
            if (isPost && what == "schedule") {
                // ★★ PERSISTED THROUGH THE CONFIG API's OWN HANDLER, not written here. The daemon
                //    owns config.json — one writer. A second writer is how a setting saved from
                //    one place gets clobbered by another, which this project has been bitten by
                //    already (the TUI vs the server, on gain).
                double v = 0;
                auto get = [&](const char* k, int dflt) {
                    return jsonNum(body, k, v) ? (int)v : dflt;
                };
                const int sh = get("updateSrvHour", -1), sd = get("updateSrvDay", -1);
                const int ah = get("updateAllHour", -1), ad = get("updateAllDay", -1);
                for (int h : { sh, ah }) if (h > 23 || h < -1) {
                    reply(400, "Bad Request", "{\"error\":\"hour must be 0-23, or -1 for off\"}");
                    return;
                }
                for (int d : { sd, ad }) if (d > 6 || d < -1) {
                    reply(400, "Bad Request", "{\"error\":\"day must be 0-6, or -1 for every day\"}");
                    return;
                }
                LocalSdrShim::setUpdateSchedule(sh, sd, ah, ad);
                LocalSdrShim::ConfigPersistFn pf;
                { std::lock_guard<std::mutex> lk(g_vsConfigMtx); pf = g_vsConfigPersist; }
                if (pf) pf("{\"updateSrvHour\":" + std::to_string(sh)
                         + ",\"updateSrvDay\":"  + std::to_string(sd)
                         + ",\"updateAllHour\":" + std::to_string(ah)
                         + ",\"updateAllDay\":"  + std::to_string(ad) + "}");
                LOGI("update schedule: vibeserver h=%d d=%d, system h=%d d=%d", sh, sd, ah, ad);
                reply(200, "OK", "{\"ok\":true}");
                return;
            }
            if (isPost && what == "action") {
                const std::string act = jsonStr(body, "action");
                std::string err;
                if (!LocalSdrShim::instance().adminAction(act, err)) {
                    reply(400, "Bad Request", "{\"error\":\"" + jsonEscape(err) + "\"}"); return;
                }
                LOGI("admin action '%s' requested by %s", act.c_str(), ip.c_str());
                reply(200, "OK", "{\"ok\":true}");
                return;
            }
            reply(404, "Not Found", "{\"error\":\"no such admin endpoint\"}");
            return;

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
            // ★★ WHAT THIS SERVER IS. Without it a client can say "a VibeServer" and no more, so
            //    the app's own About panel described the SESSION as "Local Hardware" whatever it
            //    was actually talking to. ★ Absent on a build that predates the define — clients
            //    must treat a missing version as "unknown", never as old.
#ifdef VIBESERVER_VERSION_STR
            const std::string verField = std::string(",\"version\":\"") + VIBESERVER_VERSION_STR + "\"";
#else
            const std::string verField;
#endif
            std::string body = std::string("{\"server\":\"vibeserver\",\"proto\":1,\"pin\":")
                             + (pinOn ? "true" : "false") + ",\"web\":"
                             + (g_vsWebEnabled.load() ? "true" : "false")
                             + ",\"uncompressed\":\""
                             + (um == 1 ? "choice" : um == 2 ? "compat" : "off")
                             + "\",\"local\":" + (loop ? "true" : "false")
                             + ",\"admin\":" + (adminSet ? "true" : "false") + verField
                             // ★ The owner's notice, so a client can say WHY the receiver is odd
                             //   before anybody concludes the radio is rubbish.
                             + ",\"notice\":\"" + vibeadmin::esc(g_vsNotice.current()) + "\""
                             // ★★ CONFIGURED, and it is NOT the same as `admin`. A fresh install
                             // has an admin password (the wizard makes it mandatory) and is still
                             // not set up. Clients read this to show "not set up yet — open it in
                             // a browser" rather than failing to connect against a server that is
                             // working exactly as designed.
                             + ",\"configured\":" + (g_vsConfigured.load() ? "true" : "false")
                             // ★★ OCCUPANCY IN THE IDENTITY RESPONSE. The picker already fetches
                             // this for every known server, so an IN USE badge costs nothing
                             // extra — and a public receiver that is one-client-at-a-time has to
                             // say so BEFORE someone taps it, or every busy server looks broken.
                             + ",\"busy\":" + (LocalSdrShim::instance().isBusy() ? "true" : "false")
                             // ★ HOW MANY ARE LISTENING, and the cap. "busy" is a yes/no built for
                             // a one-at-a-time receiver; on a shared one the owner wants the
                             // NUMBER, and a listener deciding whether to bother wants to know
                             // there is room. Costs nothing — the picker already fetches this.
                             + ",\"listeners\":" + std::to_string(LocalSdrShim::instance().listenerCount())
                             + ",\"maxUsers\":" + std::to_string(g_vsMaxUsers.load())
                             // ★ AND HOW MANY ARE WAITING. A full server with nobody queued and a
                             // full server with six people ahead of you are very different
                             // propositions, and only one of them is worth waiting for.
                             + ",\"waiting\":" + std::to_string(LocalSdrShim::instance().waitingCount())
                             // ★ WHAT THIS RECEIVER COVERS, in Hz. On the identity endpoint rather
                             // than tucked inside a page, because it answers the question a client
                             // has BEFORE connecting: is the band I want even here? The picker can
                             // use it, and the landing page states it in words instead of leaving
                             // the visitor to read it off the spectrogram axis (Stuart, 2026-08-06).
                             // ★ 0 when the centre is not locked — a free-running dongle has no
                             //   fixed range to promise, and inventing one would be a lie.
                             + ",\"instance\":\"" + vsInstanceId() + "\""
                             // ★★★ SAY THAT WE ARE A FRONT DOOR, ON THE ENDPOINT EVERY CLIENT
                             //     ALREADY PROBES. Without this a multi-radio server is
                             //     INDISTINGUISHABLE from an ordinary one here — same shape, same
                             //     fields — so a client connects believing it has found a
                             //     receiver and only discovers otherwise when its WebSocket is
                             //     refused with a bare 1006 and no error text. That is exactly
                             //     how VibeSDR 10.0 fails against a V3 server, and it reads as
                             //     "the server is down" rather than "choose a radio first".
                             //     ★★ Cheap on purpose: the picker already fetches this file, so
                             //        knowing costs no extra round trip. /vibeserver/radios is
                             //        then fetched only when there IS something to choose.
                             //     ★ Absent on a radio and on every older server, so a client
                             //       must read a MISSING key as "ordinary receiver" — which is
                             //       what `!== true` gives for free.
                             + (frontDoorOnly ? std::string(",\"frontDoor\":true") : std::string())
                             + ",\"rangeLo\":" + std::to_string((long long)(g_vsLockedCentre.load() > 0
                                   ? g_vsLockedCentre.load() - LocalSdrShim::instance().captureSpanHz() / 2 : 0))
                             + ",\"rangeHi\":" + std::to_string((long long)(g_vsLockedCentre.load() > 0
                                   ? g_vsLockedCentre.load() + LocalSdrShim::instance().captureSpanHz() / 2 : 0))
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
        } else if (reqLine.rfind("GET /vibeserver/conditions", 0) == 0) {
            // ★★ BOTH HALVES IN ONE REPLY, because the whole point is the comparison: what the
            //    solar numbers SUGGEST beside what this receiver can actually HEAR. Two endpoints
            //    would let a page render half of it and imply the other half agrees.
            // ★ `measured` is empty when the captured span holds no FT8 slot — an FM profile has
            //   nothing to say about HF, and saying nothing is the correct answer. The page must
            //   render that as absence, never as zeroes.
            LocalSdrShim::SolarFn sfn;
            { std::lock_guard<std::mutex> lk(g_vsConfigMtx); sfn = g_vsSolarFn; }
            std::string body = "{\"measured\":" + bandMeasJson();
            if (sfn) { const std::string sol = sfn(); if (!sol.empty()) body += ",\"solar\":" + sol; }
            body += "}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close(); return;

        } else if (reqLine.rfind("GET /vibeserver/stationlogo", 0) == 0) {
            // ★ Station artwork by PI/ECC/frequency — see setStationLogoHandler. Answers {} when
            //   this build has no handler or the station is not in RadioDNS, so the client can
            //   fall back without having to tell the two cases apart.
            StationLogoFn fn;
            { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsStationLogoFn; }
            const std::string pi  = queryParam(reqLine, "pi");
            const std::string ecc = queryParam(reqLine, "ecc");
            double hz = atof(queryParam(reqLine, "freq").c_str());
            std::string url;
            if (fn && !pi.empty() && !ecc.empty() && hz > 0) url = fn(pi, ecc, hz);
            const std::string body = url.empty() ? "{}" : "{\"logo\":\"" + vibeadmin::esc(url) + "\"}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\nCache-Control: max-age=3600\r\n"
                          "Connection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close();
        } else if (reqLine.rfind("GET /vibeserver/radios", 0) == 0) {
            vsNoteVisitor(sock->peerAddress());
            // ★★ WHAT ELSE IS ON THIS MACHINE. The landing page lists every radio the owner has
            //    enabled and configured, with the port each answers on, so one address is enough
            //    to reach all of them.
            // ★ Open, like the rest of the landing data: a listener choosing between two receivers
            //   needs to see both before they have connected to either.
            LocalSdrShim::RadiosFn rfn;
            { std::lock_guard<std::mutex> lk(g_vsConfigMtx); rfn = g_vsRadiosFn; }
            const std::string body = rfn ? rfn() : std::string("{\"radios\":[]}");
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(body.size()) + "\r\n\r\n" + body);
            sock->close(); return;

        } else if (reqLine.rfind("GET /vibeserver/rtl-serial", 0) == 0) {
            // ★ Admin-gated like the write: it names the hardware on this machine, which is not a
            //   visitor's business, and the page that asks is already signed in.
            if (!adminOkFor(reqLine, sock)) {
                sock->sendstr("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                sock->close(); return;
            }
            LocalSdrShim::RtlSerialStatusFn sf;
            { std::lock_guard<std::mutex> lk(g_vsConfigMtx); sf = g_vsRtlSerialStatusFn; }
            const std::string j = sf ? sf() : std::string("{\"pending\":false,\"bus\":[]}");
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(j.size()) + "\r\n\r\n" + j);
            sock->close(); return;
        } else if (reqLine.rfind("POST /vibeserver/rtl-serial", 0) == 0) {
            // ★★★ ADMIN ONLY, AND NOT MERELY BECAUSE IT IS A SETTING. This writes to a dongle's
            //     EEPROM: interrupted, it can leave the device unusable. Everything protective the
            //     command-line version does — refuse a chip we cannot fully parse, take a backup
            //     first, verify afterwards — is in the daemon handler and is NOT re-implemented
            //     here. The browser supplies the confirmation the terminal asked for by typing.
            if (!adminOkFor(reqLine, sock)) {
                sock->sendstr("HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                sock->close(); return;
            }
            std::string body;
            if (contentLength > 0 && contentLength < 4096) {
                std::vector<char> buf((size_t)contentLength + 1, 0);
                int got = 0;
                while (got < (int)contentLength) {
                    const int n = sock->recv((uint8_t*)buf.data() + got, (size_t)contentLength - got, 5000);
                    if (n <= 0) break;
                    got += n;
                }
                body.assign(buf.data(), (size_t)got);
            }
            const std::string want = jsonStr(body, "serial");
            LocalSdrShim::RtlSerialFn fn;
            { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsRtlSerialFn; }
            std::string msg;
            bool ok = false;
            if (!fn) msg = "not available on this server";
            else     ok  = fn(want, msg);
            const std::string j = std::string("{\"ok\":") + (ok ? "true" : "false")
                                + ",\"message\":\"" + vibeadmin::esc(msg) + "\"}";
            sock->sendstr(std::string("HTTP/1.1 ") + (ok ? "200 OK" : "400 Bad Request")
                          + "\r\nContent-Type: application/json\r\nCache-Control: no-store"
                          "\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(j.size()) + "\r\n\r\n" + j);
            LOGI("rtl serial change requested by %s — %s", sock->peerAddress().c_str(), msg.c_str());
            sock->close(); return;
        } else if (reqLine.rfind("GET /vibeserver/eibi", 0) == 0) {
            // ★ The daemon owns the fetching (it has the filesystem and the network); the shim
            //   only exposes it, and on a phone no handler is registered so this reports
            //   unavailable rather than pretending. Same split as the config endpoints.
            const bool wantRefresh = reqLine.find("refresh=1") != std::string::npos;
            LocalSdrShim::EibiFn fn;
            { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsEibiFn; }
            if (!fn) {
                const std::string b = "{\"entries\":0,\"error\":\"not available on this server\"}";
                sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Connection: close\r\nContent-Length: " + std::to_string(b.size())
                              + "\r\n\r\n" + b);
                sock->close(); return;
            }
            // ★★ A REFRESH IS ADMIN-ONLY. It spends the owner's bandwidth and CPU and writes to
            //    their disk; READING the status is not, because a listener's client may reasonably
            //    want to know whether this receiver has a schedule at all.
            if (wantRefresh && !adminOkFor(reqLine, sock)) {
                const std::string b = "{\"entries\":0,\"error\":\"admin password required\"}";
                sock->sendstr("HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\n"
                              "Connection: close\r\nContent-Length: " + std::to_string(b.size())
                              + "\r\n\r\n" + b);
                sock->close(); return;
            }
            std::string err, updated;
            const int n = fn(wantRefresh, err, updated);
            std::string b = "{\"entries\":" + std::to_string(n)
                          + ",\"updated\":\"" + updated + "\"";
            if (!err.empty()) {
                std::string e; for (char c : err) { if (c=='"'||c=='\\') e += '\\'; if ((unsigned char)c >= 0x20) e += c; }
                b += ",\"error\":\"" + e + "\"";
            }
            b += "}";
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                          "Connection: close\r\nContent-Length: " + std::to_string(b.size())
                          + "\r\n\r\n" + b);
            sock->close(); return;

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
            // ★★★ THE ADMIN CREDENTIAL, NOT THE PIN. This was gated on vsAuthOk with a comment
            //     promising "when public servers arrive this becomes the admin credential
            //     instead". They arrived; it never moved. And the gap is not theoretical: the
            //     configuration a public receiver wants is NO PIN (anyone may listen) plus an
            //     admin password (nobody may touch the radio) — and with no PIN, vsAuthOk returns
            //     true for EVERYONE. So on every public server, any listener could add or delete
            //     the receiver's bookmarks (Stuart, 2026-08-07).
            //     ★ Writing a bookmark is changing the receiver for everybody who comes after,
            //       which puts it on the CONTROL side of the line, next to gain and bias-T.
            if (!vsAdminHttpOk(sock, reqLine)) return;   // it already sent 401

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
        // ── ★★★ THE SETTINGS PAGE, REACHABLE AFTER SETUP ────────────────────────────────
        // GET /setup serves the same page whether or not the server is configured. Without this
        // the setup page was a ONE-SHOT: once `configured` went true, GET / served the receiver
        // and there was no route back — while the page itself promised "you can change any of
        // this later from Admin". A promise the product does not keep is worse than an absent
        // feature, and the only way back would have been the TUI's reset-to-not-set-up.
        // ★ Still admin-gated in the only way that matters: the page can display, but every
        //   value on it comes from GET /vibeserver/config, which refuses without the password.
        } else if (reqLine.rfind("GET /setup", 0) == 0 && !g_vsNativeSetup.load()) {
            const std::string page = kVibeSetupPage;
            sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                          + std::to_string(page.size()) + "\r\n\r\n" + page);
            sock->close();
            return;

        // ★★★ "GET / " REQUIRED A LITERAL SPACE, so the root page 404'd for ANY query string.
        //     `/?join=1` — the link every radio card uses to open a receiver — was a 404, and the
        //     browser showed nothing at all. It looked like the routing, then the hand-off, then
        //     the prefix; it was none of those. A path and its query are separate things and the
        //     match has to say so (2026-08-08).
        // ★ Kept as a prefix test rather than a full parse: everything else here matches the same
        //   way, and one route parsing the request differently from its neighbours is its own trap.
        } else if ((reqLine.rfind("GET /", 0) == 0 &&
                    (reqLine.size() == 5 || reqLine[5] == ' ' || reqLine[5] == '?')) ||
                   reqLine.rfind("GET /index.htm", 0) == 0) {
            // ★★★ NOT SET UP YET ⇒ THE SETUP PAGE, NOT THE RECEIVER.
            // A fresh install has a radio and an admin password but no POLICY — no range, no
            // listener cap, no decision about what may be changed — and until the owner sets one
            // it has no business serving strangers. So the first visit is a sign-in and a setup
            // form, not a spectrum.
            // ★ Gated on `configured`, NOT on "is an admin password set": the first-run wizard
            //   makes the password mandatory, so that can no longer stand in for this.
            // ★★ The endpoints stay open while unconfigured — this page needs /vibeserver/auth and
            //    /vibeserver/config to work, and both are admin-gated in their own right.
            if (!g_vsConfigured.load() && !g_vsNativeSetup.load() && g_vsWebEnabled.load()) {
                const std::string page = kVibeSetupPage;
                sock->sendstr("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                              "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: "
                              + std::to_string(page.size()) + "\r\n\r\n" + page);
                sock->close();
                return;
            }
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
                  const std::string& adminToken = "",
                  // ★ A cross-process admin ticket (vibe_admin_ticket.h). The nonce handshake
                  //   above cannot work here at all when the owner authenticated at the FRONT
                  //   DOOR: that is a different process with a different nonce store.
                  const std::string& adminTicket = "",
                  // ★ Carried purely for the connection log. It is the one field that separates
                  //   "a person opened the web client" from "something is scraping us" without
                  //   guessing, and the owner reading the log is the only consumer.
                  const std::string& userAgent = "",
                  // ★★ May this connection evict the current occupant? Holding a valid admin
                  //    credential is NOT the same as asking to interrupt somebody — see where it
                  //    is used. Defaults to true so callers and clients that predate it are
                  //    unaffected.
                  bool mayEvict = true) {
        std::string acc = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t digest[20]; Sha1().hash((const uint8_t*)acc.data(), acc.size(), digest);
        sock->sendstr("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + base64(digest, 20) + "\r\n\r\n");
        // ★ Who this socket belongs to, before anything can ask. See sockSession.
        if (!session.empty())
            { std::lock_guard<std::mutex> lk(clientMtx); sockSession[sock.get()] = session; }

        // ★★★ FROM HERE ON, ONE THREAD AND ONLY ONE THREAD WRITES TO THIS SOCKET.
        // Registered immediately after the handshake, because the moment this client is published
        // into specClient/specExtra the DSP thread can send to it — and a single blocking write
        // from there is the freeze this mechanism exists to prevent.
        // ★ EVERY exit from this function must call outboxClose(sock), the refusals included:
        //   they say their piece and hang up in the next line, so without the drain the server
        //   stops explaining itself and just disconnects, which a user reads as a crash.
        outboxOpen(sock);

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
            // ★ unique_lock, not lock_guard: the queue path below hands this thread off to
            //   holdInQueue(), which needs the mutex free and takes it itself once a second.
            std::unique_lock<std::mutex> lk(clientMtx);
            const std::string me = session.empty() ? ("anon:" + sock->peerAddress()) : session;

            // ★★★ THE BAN LIST COMES BEFORE EVERYTHING — before the cooldown, before occupancy,
            //     before the queue. A banned address must not be able to hold a queue slot, and
            //     it must be refused on a completely FREE radio, which is the same reasoning the
            //     cooldown check below is built on.
            // ★★ NOT APPLIED TO THE ADMIN API (see the /vibeserver/admin/ router), deliberately.
            //    An owner who fat-fingers their own range into the ban box must still be able to
            //    reach the page that undoes it. Banning yourself out of your own admin page,
            //    from a field designed for typing addresses into, is a foreseeable accident and
            //    the recovery should not be "drive to the Pi".
            // ★ Loopback is exempt for the same reason it is exempt from the cooldown: the host
            //   listening on its own machine is not the threat model.
            if (!isLoopback(sock->peerAddress())) {
                std::string why;
                if (LocalSdrShim::isBanned(sock->peerAddress(), &why)) {
                    LOGI("%s WS refused — banned (%s)", isAudio ? "audio" : "spectrum", why.c_str());
                    // ★★ TELL THEM, rather than dropping the socket. A silent close is
                    //    indistinguishable from a broken server, so a banned user retries
                    //    forever — which costs US the connection attempts, not them. A client
                    //    that is told `banned` treats it as terminal and stops, exactly as it
                    //    does for `evicted` and `cooldown`.
                    static const char* kBanned = "{\"type\":\"banned\"}";
                    sendWs(sock, 0x1, (const uint8_t*)kBanned, strlen(kBanned));
                    lk.unlock();
                    LocalSdrShim::noteConnectionClosed(sock->peerAddress(), session, "banned");
                    outboxClose(sock);
                    return;
                }
            }

            // ★★★ WHO ARE YOU — ESTABLISHED BEFORE ANY REFUSAL THAT ADMIN OUTRANKS.
            //
            //     This verification used to sit BELOW the cooldown check, so an owner serving a
            //     cooldown was turned away before anybody looked at the credentials they had just
            //     presented: password entered, still "PLEASE WAIT — you have just had a turn"
            //     (Stuart, 2026-08-11, on the public demo). The owner cannot get into their own
            //     receiver, and nothing on screen explains why, because the password was accepted
            //     everywhere else.
            //     ★★★ IT IS THE SAME MISTAKE AS THE ONE DOCUMENTED BELOW — admin status kept being
            //         computed too late to matter ("VERIFY WHENEVER CREDENTIALS ARE PRESENTED, NOT
            //         ONLY WHEN WE ARE BUSY", 2026-08-06). That fix moved it out from under
            //         `occupied`; this one moves it above the cooldown. WHENEVER A CHECK OUTRANKS
            //         A REFUSAL, IT HAS TO RUN BEFORE IT.
            //     ★★ The BAN list stays above this, deliberately: a banned address is refused even
            //        as admin here, and the recovery is the admin PAGE, which is exempt by design
            //        (see the note above) — that is where you undo a range you banned yourself into.
            bool adminAuthed = false;
            if ((!adminNonce.empty() && !adminToken.empty()) || !adminTicket.empty()) {
                std::string secret;
                { std::lock_guard<std::mutex> al(g_vsAdminMtx); secret = g_vsAdminSecret; }
                const std::string ip = sock->peerAddress();
                const bool byTicket = !adminTicket.empty() && vsTicketOk(adminTicket);
                adminAuthed = !secret.empty() && !g_vsAuthState.blocked(ip)
                           && (byTicket
                               || (!adminNonce.empty() && !adminToken.empty()
                                   && g_vsAuthState.verify(secret, adminNonce, adminToken)));
                if (adminAuthed) g_vsAuthState.recordOk(ip);
                else             g_vsAuthState.recordFail(ip);
                // ★★★ RECORD THAT SOMEONE HELD ADMIN, AND FROM WHERE. An intruder holding a
                //     compromised password is otherwise INDISTINGUISHABLE from an ordinary
                //     listener in this log — and every protection here is one they can lift: the
                //     ban list, the frequency limits, the session limit. The owner cannot notice
                //     what is never written down (Stuart, 2026-08-12).
                // ★ Marked on VERIFICATION, not at close, so a session that is later evicted or
                //   banned still carries the fact.
                if (adminAuthed) g_vsConnLog.markAdmin(sock->peerAddress(), session);
            }

            // ★★ COOLDOWN FIRST — before occupancy. Someone serving a cooldown must be refused
            // even when the radio is FREE; that is the entire point of it. Checking occupancy
            // first would let them straight back in the instant their own timeout freed the slot.
            // ★ Loopback is never on cooldown: it is never timed out in the first place.
            // ★★ AND NEITHER IS AN AUTHENTICATED ADMIN. The cooldown shares out a scarce radio
            //    between listeners; the owner is not one of the listeners it is rationing, and
            //    they are the one person who must always be able to get in — to see what the
            //    receiver is doing, or to stop it.
            if (!isLoopback(sock->peerAddress()) && !adminAuthed) {
                const double now = Impl::nowSecs();
                const auto it = cooldownUntil.find(sock->peerAddress());
                if (it != cooldownUntil.end()) {
                    if (it->second > now) {
                        const int left = (int)(it->second - now + 0.5);
                        LOGI("%s WS refused — cooling down %ds", isAudio ? "audio" : "spectrum", left);
                        const std::string m = "{\"type\":\"cooldown\",\"secs\":"
                                            + std::to_string(left) + "}";
                        sendWs(sock, 0x1, (const uint8_t*)m.data(), m.size());
                        outboxClose(sock);   // drain, then close — see outboxClose()
                        return;
                    }
                    cooldownUntil.erase(it);   // expired — prune on the way past
                }
            }

            // ★★★ FULL, not merely OCCUPIED. With a cap above 1 a second listener is welcome —
            //     that is the whole point of the shared receiver — so the refusal only applies once
            //     every slot is taken. At --users 1 this is exactly the old behaviour.
            // ★★★ ...Locked — WE ALREADY HOLD clientMtx HERE. Calling the locking form
            //     deadlocked this thread against itself and froze the whole server for
            //     every listener. See specListenerCountLocked().
            bool occupied = isFullLocked(me);
            // ★★★ A LIVE RESERVATION REFUSES EVERYONE ELSE, EVEN THOUGH A SLOT IS FREE. That is
            //     the entire value of the queue: we told someone they were next, and this is the
            //     window in which that has to be true. Without it the freed slot goes to whoever
            //     reconnects fastest and the position we displayed was decoration.
            //     ★ Same shape as the cooldown check above — refuse on a FREE radio, deliberately.
            if (!occupied && reservedUntil > Impl::nowSecs() && reservedFor != me)
                occupied = true;
            // ★ The holder arriving CONSUMES the reservation — otherwise it keeps refusing
            //   everyone else for the rest of its window after it has already done its job.
            if (!occupied && reservedFor == me) { reservedFor.clear(); reservedUntil = 0; }

            // ★★★ ADMIN OVERRIDE — the ONE case where takeover is allowed, and it must not
            // restart the reconnect war that made takeover the wrong default everywhere else.
            // Plain takeover failed because every client auto-reconnects on close, so two of
            // them displaced each other forever. The difference here is the DISPLACED client is
            // told WHY ("evicted"), which the clients treat as terminal and do not retry — and
            // an admin arriving is a deliberate, rare act by the owner, not a race between two
            // equal listeners.
            // ★★★ VERIFY WHENEVER CREDENTIALS ARE PRESENTED, NOT ONLY WHEN WE ARE BUSY. This
            //     read `if (occupied && ...)`, so connecting as admin to an IDLE receiver never
            //     checked the password at all — and therefore never granted anything. The owner
            //     got a plain listener's session: controls locked, session timer running, and no
            //     hint as to why, because on a BUSY receiver the identical action worked. It only
            //     ever looked right in the one state anybody thought to test it in (Stuart,
            //     2026-08-06: connecting as admin should be a full upgrade "the same as if they
            //     had connected as an admin direct from the splash screen").
            // ★★ Eviction still requires `occupied` — there is nobody to evict otherwise. What
            //    changes is that ADMIN STATUS no longer rides on somebody else being here.
            // ★ adminAuthed was settled above, before the cooldown could refuse them. All that is
            //   left here is whether there is anybody to evict.
            bool override_ = false;
            if (adminAuthed || !adminTicket.empty()
                || (!adminNonce.empty() && !adminToken.empty())) {
                // ★★★ CHALLENGE-RESPONSE, NEVER THE PASSWORD ITSELF — see the verification above,
                // which now runs earlier. The first cut put the admin password in the connect URL
                // as a query parameter, which over plain HTTP puts it in the clear on the wire and
                // into every proxy log on the way (Stuart, 2026-07-27). Same VsAuth the PIN and
                // admin_unlock use, so it inherits the BRUTE-FORCE LOCKOUT too — an override
                // endpoint without one is an open guessing gallery, and unlike the PIN this one
                // displaces a listener on success.
                // Evicting only makes sense if somebody is actually in the way.
                // ★★★ AND ONLY IF THEY MEANT TO. Holding the password is not the same as asking to
                //     interrupt somebody: a client replays its stored credential on EVERY automatic
                //     reconnect, so returning from the background, or a dropped socket retrying,
                //     silently threw the current listener off. With two of the owner's own clients
                //     open on one radio they took it off each other indefinitely, each reconnect
                //     evicting the other (Stuart, 2026-08-16: "when I switch back to it it fights
                //     to take the radio back"). The comment above already says a takeover must be
                //     "a deliberate, rare act by the owner" — this is what finally makes it one.
                // ★★ ABSENT means yes, so every client written before the flag behaves exactly as
                //    it did; only a client that explicitly says 0 gets the new restraint. That is
                //    the safe default here because the alternative — absent means no — would
                //    silently disable takeover for Jr and the web client until both were updated,
                //    and a takeover that quietly stops working is the fault we have just spent
                //    three days on.
                // ★ An admin who is refused here is still an ADMIN once a slot frees: this decides
                //   only whether somebody is displaced, never what the arriving session is granted.
                override_ = adminAuthed && occupied && mayEvict;
                if (adminAuthed && occupied && !mayEvict)
                    LOGI("admin arrived without takeover intent — occupant [%s] left alone",
                         occupantSession.c_str());
                if (override_) {
                    // ★★★ THE WHOLE ID, NOT THE FIRST EIGHT. Truncated, the line read "662b1dca
                //     evicting the current occupant 662b1dca" — a session apparently evicting
                //     ITSELF, which isFullLocked() makes impossible (it excludes occupantSession
                //     == me). So the two ids differ somewhere past the eighth character, and the
                //     abbreviation hid the one fact the line existed to establish.
                // ★★ A diagnostic that shortens its evidence can turn a real difference into an
                //    apparent contradiction, and send the reader looking for a bug that is not
                //    there. Print it all; these lines are rare.
                LOGI("admin override — [%s] evicting the current occupant [%s]",
                         me.c_str(), occupantSession.c_str());
                    static const char* kEvict = "{\"type\":\"evicted\"}";
                    // ★ closeAfterFlush, not close: the whole point of this frame is that the
                    //   displaced listener learns WHY, and an aborting close threw it away.
                    if (specClient  && specClient->isOpen())
                        { sendWs(specClient,  0x1, (const uint8_t*)kEvict, strlen(kEvict)); specClient->closeAfterFlush(); }
                    if (audioClient && audioClient->isOpen())
                        { sendWs(audioClient, 0x1, (const uint8_t*)kEvict, strlen(kEvict)); audioClient->closeAfterFlush(); }
                    occupantSession.clear();
                    // ★ NOT put on cooldown. They were evicted by the owner, not caught
                    // overstaying — punishing them for someone else's decision would be wrong.
                }
            }

            if (occupied && !override_) {
                // Tell them plainly, as a WS text frame (we have already upgraded), then close. The
                // client shows "in use, try again later" and must NOT retry-storm — see the web
                // client's handling of type:"busy".
                // ★★★ SAY WHO, AND WHETHER THEY ASKED AS ADMIN. Six hours of this fault were spent
                //     guessing which client a refusal belonged to, because the line named neither
                //     the session nor the credential — so "spectrum WS refused" could equally be a
                //     stranger, the evicted browser retrying, or the app arriving under a second
                //     identity, and those need opposite fixes. The occupant's id is printed beside
                //     it: if they differ, the caller is somebody else; if they match, the occupancy
                //     test itself is wrong.
                LOGI("%s WS refused — server busy (occupant [%s], caller [%s]%s)",
                     isAudio ? "audio" : "spectrum",
                     occupantSession.c_str(), me.c_str(),
                     adminAuthed ? ", caller HELD ADMIN" : ", no credential");
                // ★★ HOLD THEM, DO NOT HANG UP. Releases clientMtx first: holdInQueue takes it
                //    itself, once a second, for the life of the wait.
                lk.unlock();
                holdInQueue(sock, me);
                return;
            }
            // ★ Start the clock on a NEW occupant only. A client opens two sockets (spectrum
            // and audio) and reconnects across blips with the same id — restarting the timer
            // on each would make the limit unenforceable, and resetting it on the second
            // socket of the same session would be a quiet bug nobody would ever see.
            const bool newOccupant = (occupantSession != me);
            if (newOccupant) {
                occupantSince   = Impl::nowSecs();
                occupantWarned  = 0;
                occupantAddr    = sock->peerAddress();
                occupantAgent   = userAgent;   // for the admin view, same as ClientDsp::agent
            }
            // ★★★ THE FIRST SOCKET TO ARRIVE IS THE ANONYMOUS ONE, AND IT NAMED THE SESSION "".
            //     The apps open their AUDIO socket natively (VibeStreamService.kt / VibePowerModule
            //     .swift) with no `client=` and no User-Agent, and the client deliberately delays
            //     the spectrum socket by a second to let the session register — so the claim above
            //     always ran on the nameless one, and the admin page rendered the empty agent as
            //     "browser / unknown". The CONNECTION LOG showed "VibeSDR/10.0.2" for the very same
            //     session, because that is written from the SPECTRUM socket instead: one session,
            //     two writers, two answers (Stuart, 2026-08-14: "that should say Client VibeSDR").
            // ★★ So let a LATER socket of the same session fill in a name we do not have. Only ever
            //    filling a BLANK: a client that named itself must not be renamed by its own second
            //    socket, or the anonymous one would overwrite the good answer just as surely.
            if (!newOccupant && occupantAgent.empty() && !userAgent.empty())
                occupantAgent = userAgent;
            occupantSession = me;   // claim (or re-affirm) the slot for this client
            // ★ A NEW CLIENT IS NOT THE ADMIN. Clearing here means an unlock cannot outlive the
            // session that earned it and be inherited by whoever connects next.
            // ★★★ ...UNLESS THEY PROVED THEY ARE. A client that arrives holding a valid admin
            //     credential is the owner, and must land in the SAME state as one who typed the
            //     password into the menu afterwards: controls unlocked, no session limit, no
            //     eviction. Before this, connecting as admin cleared the flag it had just earned,
            //     so the owner was let in past the queue and then treated as a guest — the
            //     takeover worked and nothing else did.
            // ★★★ ...BUT A SESSION'S SECOND SOCKET MUST NOT DEMOTE IT. This store ran on EVERY
            //     accept for the occupant, and the apps' audio socket carries no credential at all
            //     — so every audio (re)connect, on a blip or a resume from background, stored
            //     `false` over an admin session that had proved itself on the spectrum socket or by
            //     typing the password into the menu. The owner silently became a guest: controls
            //     relocked, and the SESSION LIMIT came back (:enforceSessionLimit,
            //     :occupantSecsLeft both read this flag). It presents as "I unlocked admin and the
            //     countdown is still running", which is exactly how this was found.
            // ★★ The rule that holds: CLEAR on a NEW occupant — an unlock must never be inherited
            //    by whoever connects next — but on the SAME session only ever RAISE. A socket that
            //    presents no credential is not making a claim about admin and has no business
            //    answering the question; a socket that presents a BAD one is not granted anything
            //    either, it simply does not revoke what another socket proved. Revocation has its
            //    own paths (relock, and a new occupant taking the chair).
            // ★ Arriving with a proved credential is a login, so it supersedes any other admin
            //   for the same reason typing the password does.
            // ★★★ INLINE, BECAUSE clientMtx IS ALREADY HELD HERE. Calling demoteOtherAdmins() —
            //     which takes that same NON-RECURSIVE mutex — deadlocked the radio's accept thread
            //     on the first admin handshake, with the lock still held, so every other thread
            //     that needed it stopped too: the radio went silent while the front door and the
            //     setup page, being a different process, carried on looking healthy. Shipped in
            //     3.1.9 and it took Stuart's demo server down within the hour.
            // ★★ THE LESSON IS NOT "BE CAREFUL". A helper that takes a lock must never be called
            //    from code that already holds it, and this file's own convention says so in three
            //    places — "Applied outside the lock: setRdsEnabled touches the pipeline, and
            //    holding clientMtx across DSP calls is how this file has deadlocked itself
            //    before." I added a locking helper and then called it from inside the one region
            //    that already had the lock.
            // ★ Flags only here. The courtesy message to the demoted client is sent by the
            //   admin_unlock path, which is called with no lock held; a socket write under
            //   clientMtx would be the same mistake in a different coat.
            // ★★★ A RECONNECT IS NOT A LOGIN. "Most recent login wins" was written for a person
            //     typing the password on a second device — but a client that reconnects replays
            //     its STORED credential every time, so a churning session re-demoted everyone
            //     else every few seconds and held admin permanently. The owner at the browser
            //     unlocked, was demoted before they could touch anything, and concluded the most
            //     recent password was being ignored (Stuart, 2026-08-16) — which it was, by a
            //     rule meant to protect exactly that person.
            // ★★ So superseding requires an ARRIVAL that changed something: a NEW session, or an
            //    eviction actually taking place. A session merely re-affirming itself proves
            //    nothing new and displaces nobody.
            if (adminAuthed && (newOccupant || override_)) {
                for (auto& kv : clientDsp) {
                    auto& c = kv.second;
                    if (!c || !c->adminOk.load()) continue;
                    if (c->spec == sock || c->audio == sock) continue;
                    // ★★★ AND NEVER DEMOTE YOURSELF. `clientDsp` is keyed by SOCKET, and a browser
                    //     opens TWO — the audio socket first, the spectrum socket about a second
                    //     later (the JS client delays it deliberately). The socket test above only
                    //     skips the entry holding THIS socket, so when the second one arrived it
                    //     found the FIRST — the same person, the same visit — decided it was
                    //     "another admin", demoted it and sent it a `superseded` frame. The owner
                    //     taking over heard audio for about a second, watched the connection meter
                    //     go red, and then had it recover (Stuart, 2026-08-16).
                    // ★★★ It got worse the moment clients started ACTING on `superseded`: the
                    //     browser now drops its credential when it hears one, so a takeover could
                    //     hand the admin session away to nobody — the tab demoting itself. A
                    //     latent bug became a real one because the frame it depended on was
                    //     finally being listened to.
                    // ★★ Session, not socket, is the unit of a listener. This is the same mistake
                    //    as [two sockets, one session]: anything that decides "is this someone
                    //    else" by socket identity gets it wrong for every client that opens two.
                    // ★ Guarded on `me` being non-empty, so two genuinely anonymous clients are not
                    //   both treated as one person by an empty-string match.
                    if (!me.empty() && c->session == me) continue;
                    c->adminOk.store(false);
                    // ★★★ AND TELL THEM. This demoted silently, so a client went on believing it
                    //     was admin — badge lit, controls unlocked, no countdown — while the
                    //     server had already taken the status away. Every control it offered was
                    //     a no-op, which reads as the SERVER being broken rather than as a
                    //     demotion that nobody mentioned.
                    // ★★ It also keeps the fight going. A client that never learns it lost still
                    //    holds a valid ticket and replays it on the next reconnect, evicting the
                    //    person who displaced it — the two ends taking the radio off each other
                    //    indefinitely. The frame is what lets a client drop the credential, so
                    //    losing admin STAYS lost until somebody deliberately unlocks again.
                    // ★ Same shape as the admin_unlock refusal (`superseded`), so a client needs
                    //   one handler for both, and an older client ignores an unknown field.
                    static const char* kSup =
                        "{\"type\":\"admin\",\"ok\":false,\"superseded\":true}";
                    if (c->spec  && c->spec->isOpen())
                        sendWs(c->spec,  0x1, (const uint8_t*)kSup, strlen(kSup));
                    if (c->audio && c->audio->isOpen())
                        sendWs(c->audio, 0x1, (const uint8_t*)kSup, strlen(kSup));
                    LOGI("admin superseded — a newer login arrived holding the password");
                }
            }
            if (newOccupant || adminAuthed) adminOk.store(adminAuthed);
            if (adminAuthed) lastAdminTouch.store(Impl::nowSecs());
            if (adminAuthed) LOGI("admin session — controls unlocked, no session limit");
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
            outboxClose(sock);   // drain, then close — see outboxClose()
            return;
        }

        // Slot won — NOW it is safe to adopt this client's bin count. Before the
        // occupancy check, a refused client corrupted the incumbent's stream.
        // ★★★ RECORD IT AGAINST THIS CLIENT, never in a global. Storing it globally is what let
        //     a watch asking for 128 cut every browser on the server down to 128 bins.
        // ★★★ ON A SHARED RECEIVER THE WIDTH IS NOT THE ASKER'S ALONE TO CHOOSE. A client may ask
        //     for up to OUT_BINS (4096), and per-client widths mean it only RECEIVES its own —
        //     but `wireBins()` sizes the one shared zoom FFT to the MAXIMUM anybody asked for,
        //     because a narrower listener can be peak-held down from a wide row and a wider one
        //     cannot be interpolated up from a narrow one. So a single 4096 request makes the DSP
        //     compute a 4096-wide FFT for EVERY listener on the radio, and the cost is not small:
        //     the note on WIRE_BINS_DEFAULT measures 4096 at 0.50 Mb/s per listener against a
        //     quarter of that at 1024, "the difference between tens of listeners and hundreds".
        //     Found on the demo's RSP1B while measuring something else — the probe asked for 4096,
        //     got it, and was loading the radio for ten other people the whole time it measured.
        // ★★ SO THE CAP IS THE SHARED CASE ONLY. With one listener and its own DSP, 4096 costs
        //    nobody but the asker and stays available — a desktop on a wide screen has a real use
        //    for it. perClientDsp() is precisely "shared receiver": a locked centre and room for
        //    more than one. Same shape as the fps fix: one listener must not spend everyone's CPU.
        if (wantBins > 0) {
            int use = wantBins;
            if (perClientDsp() && use > WIRE_BINS_DEFAULT) {
                // ★ SAY SO. A silently downgraded width is indistinguishable from a client bug,
                //   and this is a request the server is deliberately not honouring.
                LOGI("bins: asked %d, capped to %d — shared receiver (one width sizes everyone's FFT)",
                     use, WIRE_BINS_DEFAULT);
                use = WIRE_BINS_DEFAULT;
            }
            std::lock_guard<std::mutex> lk(clientMtx); clientBins[sock.get()] = use;
        }

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

        // ★ And the same for the rate: a fresh listener starts on the server default, not on
        //   whatever the previous occupant of this socket address asked for.
        if (!isAudio) {
            { std::lock_guard<std::mutex> lk(clientMtx);
              clientFps.erase(sock.get()); clientFpsAcc.erase(sock.get()); }
            recomputeEngineRate();
        }

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
            // ★★★ IN SHARED MODE EVERY LISTENER HAS THEIR OWN AUDIO SOCKET. `audioClient` is a
            //     single pointer built for a one-at-a-time receiver; here it would mean the last
            //     listener to connect took everyone's audio. Attach this socket to ITS OWN
            //     channel instead, matched on the session id.
            if (perClientDsp()) {
                bool matched = false;
                { std::lock_guard<std::mutex> lk(clientMtx);
                  for (auto& kv : clientDsp) {
                      if (kv.second->session != session || session.empty()) continue;
                      kv.second->audio = sock;
                      kv.second->wantsOpus = wantsOpus;
                      kv.second->forceMono = forceMono;
                      matched = true;
                      break;
                  }
                  // Arrived first: hold it until its spectrum socket turns up.
                  if (!matched) pendingAudio[session] = sock; }
                LOGI("audio WS connected (%s, codec=%s)",
                     matched ? "own channel" : "waiting for its spectrum socket",
                     wantsOpus ? "opus" : "pcm");
            }
            { std::lock_guard<std::mutex> lk(clientMtx);
              stale = perClientDsp() ? nullptr : audioClient;
              if (!perClientDsp()) audioClient = sock; }
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
            // ★★★ FIRST LISTENER IS THE PRIMARY; the rest join the list. Every existing path uses
            //     `specClient`, so the primary keeps them all working unchanged, and the extras get
            //     the frames and the config in the broadcast loops.
            bool asExtra = false;
            bool firstOfSession = false;
            { std::lock_guard<std::mutex> lk(clientMtx);
              // ★ Nobody listening yet? Then this is a NEW SESSION and the owner's landing
              //   frequency applies. Counted under the lock, applied outside it.
              firstOfSession = (specListenerCountLocked() == 0);
              // prune anything that has gone away since
              specExtra.erase(std::remove_if(specExtra.begin(), specExtra.end(),
                  [](const std::shared_ptr<net::Socket>& c){ return !c || !c->isOpen(); }),
                  specExtra.end());
              if (specClient && specClient->isOpen() && g_vsMaxUsers.load() > 1) {
                  specExtra.push_back(sock); asExtra = true;
              } else {
                  stale = specClient;
                  specClient = sock;
              } }
            // ★★ THE CONNECTION LOG STARTS HERE — at the SPECTRUM socket, not the audio one.
            //    A slot is a spectrum listener; the audio socket is optional and arrives second,
            //    so logging both would double-count every ordinary browser.
            LocalSdrShim::noteConnectionOpened(sock->peerAddress(), session, userAgent,
                                               vsCountry(sock->peerAddress()));
            // ── ★★★ LAND A NEW SESSION WHERE THE OWNER SAID ────────────────────────────
            // Before sendConfig, so the client is told the frequency it is actually on rather
            // than being told one thing and then moved.
            // ★★★ ONCE PER SESSION, NOT ONCE PER SOCKET THAT FINDS THE ROOM EMPTY.
            //     `firstOfSession` is "no spectrum listener right now", and a client opens THREE
            //     sockets (spectrum, audio, decoder) and reopens them across blips — so the
            //     landing fired again and again, and each time it threw away the tune the listener
            //     had just restored. The log said it plainly: mode=wfm, then "landing on 648.000
            //     kHz am", then mode=wfm, then landing again, four times in ninety seconds. The
            //     listener saw 107.4 FM on the dial and heard Radio Caroline on 648 (Stuart,
            //     2026-08-15) — and tuning by hand "fixed" it only until the next socket landed
            //     them back.
            // ★★ THE LANDING IS FOR A NEW SESSION, which is what its own comment says, so the test
            //    is the SESSION and not the socket count. Remembered by id: the same listener
            //    returning is not a new arrival, and a genuinely new one still gets the owner's
            //    chosen frequency exactly once.
            // ★★★ AND NEVER LAND AN OWNER TAKING THEIR RECEIVER BACK. The landing frequency is for
            //     a NEW LISTENER who has not said where they want to be — it is the owner's answer
            //     to "where should a stranger start". An admin arriving with the password is the
            //     opposite of that: they had a session, they are resuming it, and they have just
            //     proved who they are. Landing them dropped the owner onto their own default —
            //     "Caroline starts playing although I'm tuned to FM" — with the dial showing one
            //     frequency and the audio on another (Stuart, 2026-08-15).
            // ★★ It is also the case where landing does the most damage: a takeover is the one
            //    arrival that ALREADY has a tune worth keeping, and the client then has to fight
            //    the server to get it back. Four client-side workarounds went into that fight
            //    before it was clear the server should simply not start it.
            // ★ adminOk was settled for this client a few lines above (the accept path stores it
            //   from the handshake credential), and it is in scope here where `adminAuthed` is not.
            // ★★★ AND NOT IF THIS SESSION HAS ALREADY TUNED. The audio socket opens first and
            //     carries the tune, so by the time the spectrum socket gets here the listener may
            //     have said exactly where they want to be — see preTunedSession. The landing is
            //     for someone who has NOT.
            bool preTuned;
            { std::lock_guard<std::mutex> lk(clientMtx);
              preTuned = !session.empty() && preTunedSession == session; }
            if (preTuned)
                LOGI("session [%s] already tuned to %.3f kHz %s — landing skipped",
                     session.c_str(), audioFreq.load() / 1e3, mode.c_str());
            if (firstOfSession && landedSession != session && !adminOk.load() && !preTuned) {
                landedSession = session;
                // ★ See the loopback warning in the watchdog: a proxy or tunnel connects from
                //   127.0.0.1, and loopback is exempt from the session limit.
                if (sock) {
                    const std::string pa = sock->peerAddress();
                    if (pa == "127.0.0.1" || pa == "::1" || pa.rfind("127.", 0) == 0)
                        g_vsLoopbackSessions.fetch_add(1);
                }
                const double hz = g_vsLandingHz.load();
                std::string lm; { std::lock_guard<std::mutex> lk(g_vsLandingMtx); lm = g_vsLandingMode; }
                if (!lm.empty() && lm != mode) { mode = lm; buildAudio(); }
                if (hz > 0) retune(hz);
                if (hz > 0 || !lm.empty())
                    LOGI("new session — landing on %.3f kHz %s", hz / 1e3,
                         lm.empty() ? mode.c_str() : lm.c_str());
            }
            // ★★★ THIS LISTENER'S OWN CHANNEL. Created before sendConfig so the config it is sent
            //     already describes its own VFO rather than somebody else's.
            if (perClientDsp()) {
                auto c = std::make_shared<ClientDsp>();
                c->owner = this;
                // ★ The owner's configured defaults are what a new listener starts from.
                c->wspOn.store(weakProcOn.load()); c->imsOn.store(imsOn.load());
                c->ceqOn.store(ceqOn.load());      c->nbOn.store(nbOn.load());
                // ★ The credential proved at the handshake belongs to THIS listener. The accept
                //   path settled it into the radio-wide flag a few lines earlier (it is still the
                //   right answer for a receiver with no per-client DSP), so it is read from there
                //   rather than threading the local through two scopes.
                c->adminOk.store(adminOk.load());
                if (c->adminOk.load()) c->lastAdminTouch.store(Impl::nowSecs());
                c->spec = sock;
                c->session = session;
                c->agent = userAgent;
                // ★★★ BUILD THE CHANNEL WHERE THE LISTENER ASKED TO BE, not at the landing.
                //     This was the SECOND half of the same fault: even with the landing gate
                //     skipped, a channel created from `g_vsLandingHz` cuts the spectrum at 648 kHz
                //     for someone who tuned to 90.1 a moment ago on their audio socket. The tune
                //     they sent is already in `audioFreq`/`mode` — the shared handler applied it —
                //     so adopt that rather than the owner's default.
                if (preTuned) {
                    c->vfoHz = audioFreq.load();
                    c->mode  = mode;
                } else {
                    c->vfoHz = g_vsLandingHz.load() > 0 ? g_vsLandingHz.load() : audioFreq.load();
                    { std::lock_guard<std::mutex> lk(g_vsLandingMtx);
                      c->mode = g_vsLandingMode.empty() ? mode : g_vsLandingMode; }
                }
                c->bwHz = paramsFor(c->mode).bandwidth;
                { std::lock_guard<std::mutex> lk(clientMtx); clientDsp[sock.get()] = c; }
                clientRetune(c.get());
                startClientThread(c);            // its own DSP thread, from here on
                // ★ An audio socket may already be waiting: a browser opens both at once and the
                //   order is not guaranteed. Adopt it rather than leaving the listener silent.
                adoptAudioForSession(session);
                LOGI("listener %s: own channel at %.3f kHz %s",
                     session.empty() ? "(anon)" : session.c_str(), c->vfoHz / 1e3, c->mode.c_str());
            }
            sendConfig(sock); sendHwInfo(sock);
            broadcastUsers();          // ★ everyone learns someone joined, including the joiner
            if (asExtra)
                LOGI("spectrum WS connected — listener %d of %d",
                     specListenerCount(), g_vsMaxUsers.load());
            else
                LOGI("spectrum WS connected");
            // ★★★ AN EXTRA LISTENER USED TO `return` HERE — AND THAT WAS THREE BUGS AT ONCE.
            // Returning skipped the read loop below, so for every listener after the first:
            //   1. NOTHING WAS EVER READ from the socket. Its control messages were ignored, so a
            //      second listener could not even be pinged, and anything it sent sat unread until
            //      the kernel receive buffer filled.
            //   2. NO LIVENESS. The ping/pong probe lives in that loop, so a VANISHED extra (a
            //      suspended phone, a dropped link with no FIN) was never detected — `isOpen()`
            //      stays true on a peer that is simply gone, so it held a slot forever. On a server
            //      advertising a user cap, leaked slots eventually refuse everybody.
            //   3. NO TEARDOWN. The cleanup at the end of this function never ran for extras.
            // ★★ Falling through fixes all three at once, because that loop is exactly where a
            //    client's read, liveness and cleanup already live. An extra is a listener like any
            //    other — the ONLY thing that differs is which pointer holds it.
            // ★ This is why "multi-listener is BUILT BUT UNPROVEN" was the right label: a second
            //   listener had never been driven far enough to notice it was write-only.
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
        std::shared_ptr<ClientDsp> goneDsp;
        { std::lock_guard<std::mutex> lk(clientMtx);
          if (specClient == sock) {
              // Promote the next listener so the survivors keep their stream — every path that
              // uses `specClient` would otherwise go dark for everyone when the first one leaves.
              specClient = nullptr;
              for (auto& c : specExtra)
                  if (c && c->isOpen()) { specClient = c; c = nullptr; break; }
          }
          specExtra.erase(std::remove_if(specExtra.begin(), specExtra.end(),
              [&](const std::shared_ptr<net::Socket>& c){ return !c || c == sock || !c->isOpen(); }),
              specExtra.end());
          clientBins.erase(sock.get());     // its width leaves with it
          // ★★★ AND ITS RATE. This is the half that made the old bug permanent: a listener could
          //     slow the radio and then leave, and nothing recomputed anything, so the waterfall
          //     stayed at 5 fps for everyone still watching until somebody happened to ask for
          //     fast. Measured: bystander 24.5 -> 4.9 fps, still 4.9 after the slow client had
          //     disconnected. The recompute is below, OUTSIDE this lock.
          clientFps.erase(sock.get());
          clientFpsAcc.erase(sock.get());
          // ★ Close the log entry for whoever this socket was. The reason is "closed" — the
          //   paths that end a session for a REASON (kicked, banned, timeout) each record their
          //   own before getting here, and ConnLog::close only ever fills the most recent
          //   still-open record, so this cannot overwrite one of those.
          // ★★★ CLOSE IT EVEN WHEN THERE IS NO ClientDsp, WHICH IS THE COMMON CASE. This was
          //     gated on finding a per-client DSP for the socket — but `clientDsp` is only
          //     populated when perClientDsp() holds (a LOCKED centre with room for more than one).
          //     On a DIRECT-mode radio there is never an entry, so the close was never recorded
          //     and every connection stayed OPEN IN THE LOG FOR EVER. The admin table then shows
          //     "connected now" for everyone who has ever connected since the last restart —
          //     phantom listeners, and a country list that only ever grows (Stuart, 2026-08-10:
          //     "it lists IPs being connected now even though nobody is connected", and countries
          //     that changed between viewings). It bit exactly the radios in single mode and
          //     spared the shared one, which is why it looked intermittent.
          // ★★ The OPEN above is unconditional; a close that is conditional on anything the open
          //    did not require is a leak by construction. Match them.
          // ★ Session id when we have one, empty when we do not: ConnLog::close fills the most
          //   recent still-open record for the address either way — the same call the idle-timeout
          //   path already makes with no session at all.
          // ★ Spectrum only, to mirror the open: the audio socket is optional and arrives second,
          //   so closing on it would end the record while the listener is still here.
          if (!isAudio) {
            auto it = clientDsp.find(sock.get());
            const std::string sess = (it != clientDsp.end() && it->second)
                                   ? it->second->session : std::string();
            LocalSdrShim::noteConnectionClosed(sock->peerAddress(), sess, "closed");
          }
          // ★ The channel goes with the listener: its pipeline, its slice, its encoder.
          // ★ Lift it out under the lock, stop its thread OUTSIDE — joining a thread while
          //   holding clientMtx would deadlock against anything that thread wants.
          { auto it = clientDsp.find(sock.get());
            if (it != clientDsp.end()) { goneDsp = it->second; clientDsp.erase(it); } }
          sockSession.erase(sock.get());
          for (auto it = pendingAudio.begin(); it != pendingAudio.end(); ) {
              if (it->second == sock) it = pendingAudio.erase(it); else ++it;
          }
          for (auto& kv : clientDsp) if (kv.second->audio == sock) kv.second->audio = nullptr;
          if (audioClient == sock) audioClient = nullptr;
          // Free the slot once BOTH of the occupant's sockets are gone (a browser closing one tab
          // drops both). Until then a momentary spectrum reconnect must not surrender the slot to a
          // waiting device. A refused socket never reached here (it returned early), so it can't
          // clear an occupancy it never held.
          const bool specGone  = !specClient  || !specClient->isOpen();
          const bool audioGone = !audioClient || !audioClient->isOpen();
          if (specGone && audioGone) { occupantSession.clear(); bothGone = true; } }
        outboxClose(sock);   // drain, then close — see outboxClose()
        // ★ The slow listener has gone: the survivors get their rate back. OUTSIDE the lock —
        //   recomputeEngineRate() takes clientMtx, and a helper that locks must never be called
        //   from a scope that holds it (see specListenerCountLocked's deadlock note).
        if (!isAudio) recomputeEngineRate();
        broadcastUsersSafe();      // ★ someone left — tell whoever is still here
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
            if (stillEmpty) armIdlePark();
            else LOGI("not parking — a new listener arrived while this socket was closing");
        }
        LOGI("%s WS disconnected", isAudio ? "audio" : "spectrum");
    }

    void startDecoder(const std::string& msg) {
        std::string ext = jsonStr(msg, "extension_name");
        { std::lock_guard<std::mutex> lk(decoderMtx); currentDecoder = ext; }
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
        // ★★★ WEFAX WAS NEVER DISPATCHED. startWefax() has always existed, fully written —
        //     config parsing, onLine/onStart/onStop, the image framing — and NOTHING EVER CALLED
        //     IT. `extension_name: "wefax"` fell through to the `ext != "fsk" && !navtex` guard
        //     below and returned, so no WefaxDecoder was ever constructed and not one line of
        //     image was ever produced.
        //     ★★ It did not look like an absent feature, which is why it survived: the client
        //     drew the WEFAX panel, said "decoding…", and waited. Stuart, 2026-08-06 — "WEFAX
        //     completely missed an entire transmission", then, having caught the next one from
        //     the very start, "wefax still not working". It was never running.
        //     ★ Same family as `doAdminUnlock` being defined and never called, and as the admin
        //       `adminOk` flag being sent and never read: complete, correct code with no caller.
        //       A function nobody calls is invisible to every test that exercises the callers.
        if (ext == "wefax") { startWefax(msg); return; }
        // ★ MSF (60 kHz) and DCF77 (77.5 kHz). Tune them in CW: the carrier arrives as a beat note
        //   whose AMPLITUDE carries the code, which is what the decoder reads.
        // ★ ONE extension, the station as a parameter — the client sends a preset, exactly as it
        //   does for RTTY's shift and baud. The old per-station names still work so a client that
        //   predates the change is not broken by it.
        if (ext == "time") {
            std::string st = jsonStr(msg, "station");
            if (st.empty()) st = "msf";
            startTime(msg, st);
            return;
        }
        if (ext == "msf" || ext == "dcf77" || ext == "rwm" || ext == "wwv" || ext == "wwvb") {
            startTime(msg, ext); return;
        }
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
        // ★ ONE DECODER OWNS THE AUDIO. feedDecoder dispatches in a fixed order, so leaving an
        //   old image or time decoder alive would silently take priority over the one just asked
        //   for — the panel would sit blank while a decoder nobody selected ran instead.
        delete wefax;   wefax   = nullptr;
        delete sstv;    sstv    = nullptr;
        delete timeDec; timeDec = nullptr;
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
        delete sstv;    sstv    = nullptr;
        delete timeDec; timeDec = nullptr;
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
    /** ★ MSF / DCF77. Output goes down the SAME text channel RTTY uses, so it needs no new client
     *  protocol and appears in the decoder panel that already exists.
     *  ★★ IT REPORTS WHILE IT IS STILL WAITING. A minute is 59 bits, so a listener who has just
     *     tuned may wait a full minute before anything is certain — and a panel that says nothing
     *     for a minute is indistinguishable from one that is broken. So the state and the measured
     *     carrier margin go out too: "searching, carrier +19 dB" is the difference between "keep
     *     waiting" and "point the aerial somewhere else". */
    void startTime(const std::string& msg, const std::string& which) {
        (void)msg;
        std::lock_guard<std::mutex> lk(decoderMtx);
        delete decoder; decoder = nullptr;
        delete wefax;   wefax   = nullptr;
        delete sstv;    sstv    = nullptr;
        delete timeDec;
        const TimeDecoder::Station st =
              which == "dcf77" ? TimeDecoder::Station::DCF77
            : which == "rwm"   ? TimeDecoder::Station::RWM
            : which == "wwvb"  ? TimeDecoder::Station::WWVB
            : which == "wwv"   ? TimeDecoder::Station::WWV
                               : TimeDecoder::Station::MSF;
        timeDec = new TimeDecoder(48000, st);
        std::string name = which; for (auto& c : name) c = (char)toupper((unsigned char)c);
        timeDec->onTime = [this, name](const TimeDecoder::TimeStamp& t) {
            static const char* kDay[8] = { "", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s  %s %04d-%02d-%02d %02d:%02d  %s%s\n",
                          name.c_str(), kDay[t.weekday >= 1 && t.weekday <= 7 ? t.weekday : 0],
                          t.year, t.month, t.day, t.hour, t.minute,
                          t.dst ? "(summer time)" : "",
                          t.leapSecondPending ? " LEAP SECOND PENDING" : "");
            std::lock_guard<std::mutex> bl(decBufMtx);
            decTextBuf += buf;
        };
        // ★★★ FILL THE FIELDS AS THEY ARRIVE. A minute is 59 bits, so waiting for the whole thing
        //     leaves the panel blank for a minute — and blank is indistinguishable from broken
        //     (Stuart, 2026-08-11, watching MSF read cleanly and show nothing). Sent as a
        //     replace-in-place line so the panel updates rather than scrolling 59 times a minute.
        //     ★★ It is the best diagnostic here too: seeing the year fill correctly and the hour
        //        come out wrong says exactly where the framing slipped, which a pass/fail at the
        //        end of the minute never could.
        timeDec->onPartial = [this, name](const TimeDecoder::Partial& p) {
            // ★ Flush any callsign heard in the last second, so it appears as it is sent rather
            //   than only once a buffer fills.
            { std::lock_guard<std::mutex> bl(decBufMtx);
              if (morseWord_.size() >= 3) { decTextBuf += "RWM ID: " + morseWord_ + "\n"; morseWord_.clear(); } }
            char buf[200];
            char yy[8], mo[4], dd[4], hh[4], mi[4];
            std::snprintf(yy, sizeof(yy), p.year  ? "%04d" : "----", p.t.year);
            std::snprintf(mo, sizeof(mo), p.month ? "%02d" : "--",   p.t.month);
            std::snprintf(dd, sizeof(dd), p.day   ? "%02d" : "--",   p.t.day);
            std::snprintf(hh, sizeof(hh), p.hour  ? "%02d" : "--",   p.t.hour);
            std::snprintf(mi, sizeof(mi), p.minute? "%02d" : "--",   p.t.minute);
            std::snprintf(buf, sizeof(buf), "\r%s  %s-%s-%s %s:%s   second %02d/59",
                          name.c_str(), yy, mo, dd, hh, mi, p.second);
            std::lock_guard<std::mutex> bl(decBufMtx);
            decTextBuf += buf;
        };
        timeDec->onState = [this, name](TimeDecoder::State st) {
            const char* w = st == TimeDecoder::State::NoSignal ? "no carrier"
                          : st == TimeDecoder::State::Searching ? "searching for the minute"
                          : st == TimeDecoder::State::Reading   ? "reading the minute"
                                                                : "locked";
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[%s] %s — carrier %+.0f dB\n",
                          name.c_str(), w, timeDec ? timeDec->snrDb() : 0.0);
            std::lock_guard<std::mutex> bl(decBufMtx);
            decTextBuf += buf;
        };
        // ★★ RWM's CALLSIGN, which is the only proof it is being heard. Buffered into a word and
        //    flushed on a pause, so the panel shows "RWM" rather than one letter per line.
        timeDec->onMorse = [this](char c) {
            std::lock_guard<std::mutex> bl(decBufMtx);
            morseWord_ += c;
            if (morseWord_.size() >= 24) { decTextBuf += "RWM ID: " + morseWord_ + "\n"; morseWord_.clear(); }
        };

        // ★★ SAY UP FRONT WHEN THERE IS NOTHING TO WAIT FOR. RWM transmits markers and a Morse
        //    callsign and NO timecode, so a panel that sat there "reading the minute" for ever
        //    would look broken when it was working perfectly. Tell the user what it can do.
        if (!timeDec->carriesTimeCode()) {
            std::lock_guard<std::mutex> bl(decBufMtx);
            decTextBuf += "RWM sends second and minute markers and a Morse callsign — it carries "
                          "NO date or time code, so none can be shown. Use it for propagation and "
                          "calibration.\n";
        }
        LOGI("time decoder: %s (tune it in CW)", name.c_str());
    }

    void startSstv(const std::string& msg) {
        (void)msg;
        std::lock_guard<std::mutex> lk(decoderMtx);
        delete decoder; decoder = nullptr;
        delete wefax;   wefax = nullptr;
        delete sstv;
        delete timeDec; timeDec = nullptr;   // ★ only one decoder owns the audio
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
        { std::lock_guard<std::mutex> lk(decoderMtx); currentDecoder.clear(); }
        rx.setRdsNoiseCorrection(false);   // nobody looking: stop paying for it
        std::lock_guard<std::mutex> lk(decoderMtx);
        delete decoder; decoder = nullptr;
        delete wefax;   wefax = nullptr;
        delete sstv;    sstv = nullptr;
        delete timeDec; timeDec = nullptr;
        { std::lock_guard<std::mutex> bl(decBufMtx); decTextBuf.clear(); }
    }

    /** @param session the listener this decoder socket belongs to. ★ It never carried one: with
     *  a single VFO there was only one thing it could possibly be decoding. Per-client tuning
     *  makes "whose audio?" a real question, and the session is the only thing that answers it. */
    void acceptDxcluster(std::shared_ptr<net::Socket> sock, const std::string& wsKey,
                         const std::string& session = "") {
        std::string acc = wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        uint8_t digest[20]; Sha1().hash((const uint8_t*)acc.data(), acc.size(), digest);
        sock->sendstr("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: " + base64(digest, 20) + "\r\n\r\n");
        // ★★ The decoder sockets are written from the DECODE threads (WEFAX lines, SSTV rows,
        // FT8 spots), so they need the same protection as the spectrum path — a browser tab that
        // stops draining its decoder stream must not stall the decoders for everyone else.
        outboxOpen(sock);
        { std::lock_guard<std::mutex> lk(clientMtx); dxClient = sock; decoderSession = session; }
        decoderAttached.store(true, std::memory_order_relaxed);
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
          if (stillCurrent) { dxClient = nullptr; decoderSession.clear(); } }
        if (stillCurrent) { decoderAttached.store(false, std::memory_order_relaxed);
                            stopDecoder(); stopSpots(); }
        outboxClose(sock);
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

    /** Push IQ through the shared forward FFT and hand every listener their own channel.
     *  ★ Copies the client list under the lock and works OUTSIDE it: this runs on the DSP thread,
     *  and holding clientMtx across a whole block of demodulation would stall every connect and
     *  disconnect behind it. */
    /** This listener's channel, by EITHER of its sockets. Null when per-client DSP is off.
     *  ★★★ EITHER, and that is not defensive coding — the two clients differ.
     *  The web client sends its control messages over the SPECTRUM socket; the iOS app sends
     *  `tune` over the AUDIO socket, from VibePowerModule, because that is where its native audio
     *  path lives. Keyed on the spectrum socket alone, the app's tune found no listener, fell
     *  through to the shared handler and moved a VFO nobody was listening to: the waterfall
     *  followed the dial and the audio stayed where it started (Stuart, 2026-08-05, on an iPhone).
     *  ★ A lookup that only works for the client you happened to test with is a trap for whoever
     *    adds the third one. */
    std::shared_ptr<ClientDsp> dspFor(const std::shared_ptr<net::Socket>& sock) {
        if (!sock) return nullptr;
        std::lock_guard<std::mutex> lk(clientMtx);
        auto it = clientDsp.find(sock.get());
        if (it != clientDsp.end()) return it->second;
        for (auto& kv : clientDsp) if (kv.second->audio == sock) return kv.second;
        return nullptr;
    }

    /** This listener's admin state. ★ Falls back to the radio-wide flag when this receiver has no
     *  per-client DSP — on such a radio there is only ever one listener, so it IS theirs. */
    bool adminNow(const std::shared_ptr<net::Socket>& sock) {
        if (auto d = dspFor(sock)) return d->adminOk.load();
        return adminOk.load();
    }
    /** Grant or revoke it for this listener.
     *
     *  ★★★ ADMIN IS EXCLUSIVE, AND THE MOST RECENT LOGIN WINS. Stuart, 2026-08-15: "if only one
     *      person can be admin, which should be the case, it should be the one who has logged in
     *      most recently. Just in case I leave the house and forget I've left it open on the Mac,
     *      I can then use the app on my iPhone to be the admin."
     *  ★★★ THAT IS A REAL SCENARIO AND IT DECIDES THE DESIGN. An owner cannot always reach the
     *      machine that holds the unlock, so an admin session left open somewhere else must not be
     *      able to lock the owner out of their own receiver. Whoever proves the password last has
     *      demonstrated they hold it; the earlier session has demonstrated nothing since.
     *  ★★ DEMOTED, NOT DISCONNECTED. The previous admin keeps listening and simply becomes an
     *     ordinary listener — controls locked, and the session limit applies to them again. Being
     *     out-ranked is not a reason to take somebody's audio away; that is what eviction is for,
     *     and it is a separate, deliberate act.
     *  ★ This replaces the per-listener-admin model of an hour earlier, which let two owners hold
     *    it at once. Per-listener STATE is still right — each listener's unlock, idle clock and
     *    countdown exemption are their own — but the POLICY on top of it is exclusive.
     */
    void setAdminNow(const std::shared_ptr<net::Socket>& sock, bool on) {
        if (on) demoteOtherAdmins(sock);
        if (auto d = dspFor(sock)) {
            d->adminOk.store(on);
            if (on) d->lastAdminTouch.store(Impl::nowSecs());
            // ★ The radio-wide flag follows the current admin, because the paths with no
            //   particular listener in mind (the /vibeserver.json probe) still read it.
            adminOk.store(on);
            if (on) lastAdminTouch.store(Impl::nowSecs());
            return;
        }
        adminOk.store(on);
        if (on) lastAdminTouch.store(Impl::nowSecs());
    }

    /** Take admin away from every listener except `keep`, and tell each of them why. */
    void demoteOtherAdmins(const std::shared_ptr<net::Socket>& keep) {
        std::vector<std::shared_ptr<net::Socket>> told;
        {
            std::lock_guard<std::mutex> lk(clientMtx);
            for (auto& kv : clientDsp) {
                auto& c = kv.second;
                if (!c || !c->adminOk.load()) continue;
                if (keep && (c->spec == keep || c->audio == keep)) continue;
                c->adminOk.store(false);
                if (c->spec) told.push_back(c->spec);
            }
        }
        // ★★ SAY SO. A control that silently stops working reads as a bug — the same reasoning as
        //    the idle re-lock, which learnt this the hard way. `ok:false` greys the controls
        //    through the path the client already has; `superseded` is what lets it explain.
        for (auto& sc : told) {
            if (sc && sc->isOpen())
                sendText(sc, "{\"type\":\"admin\",\"ok\":false,\"superseded\":true}");
            LOGI("admin superseded — an owner unlocked more recently elsewhere");
        }
    }
    /** Stamp the idle clock for whoever sent a command. */
    void touchAdmin(const std::shared_ptr<net::Socket>& sock) {
        if (auto d = dspFor(sock)) { if (d->adminOk.load()) d->lastAdminTouch.store(Impl::nowSecs()); return; }
        if (adminOk.load()) lastAdminTouch.store(Impl::nowSecs());
    }

    /** The listener whose audio drives the decoders. ★ Falls back to the FIRST listener when the
     *  decoder socket carried no session id — every client shipped before this did exactly that,
     *  and without the fallback their decoders simply never receive audio: RDS, WEFAX, SSTV and
     *  FT8 all silently dead (Stuart, 2026-08-05: "none of the decoders are working"). */
    std::shared_ptr<ClientDsp> decoderOwner() {
        std::lock_guard<std::mutex> lk(clientMtx);
        if (!decoderSession.empty())
            for (auto& kv : clientDsp)
                if (kv.second->session == decoderSession) return kv.second;
        if (specClient) {
            auto it = clientDsp.find(specClient.get());
            if (it != clientDsp.end()) return it->second;
        }
        return clientDsp.empty() ? nullptr : clientDsp.begin()->second;
    }

    /** ★★ A browser opens its spectrum and audio sockets AS A PAIR, in no guaranteed order, and
     *  they are tied together only by the session id. Whichever arrives second does the matching —
     *  so this is called from both paths. Without it a listener whose audio socket landed first is
     *  silent forever, which looks exactly like a broken receiver. */
    void adoptAudioForSession(const std::string& session) {
        if (session.empty()) return;
        std::lock_guard<std::mutex> lk(clientMtx);
        auto it = pendingAudio.find(session);
        if (it == pendingAudio.end()) return;
        for (auto& kv : clientDsp) {
            if (kv.second->session != session) continue;
            kv.second->audio = it->second;
            pendingAudio.erase(it);
            return;
        }
    }

    /** Do one client's channels for this block. Pure per-client work — no shared mutable state,
     *  which is exactly why it parallelises. */
    void feedOneClient(const std::shared_ptr<ClientDsp>& c, const cf32* bins, long long blockIndex) {
        std::lock_guard<std::mutex> lk(c->mtx);
        if (!c->rx || c->chanBins <= 0) return;
        const int got = chan_->extract(bins, clientCentreBin(c.get()), c->chanBins,
                                       c->slice.data(), c->ectx, blockIndex);
        if (got > 0) c->rx->feed(c->slice.data(), got);
        // ★ And the VIEW channel, when this listener is drawing its own waterfall.
        if (c->viewRx && c->viewChanBins > 0) {
            const int gv = chan_->extract(bins, clientViewCentreBin(c.get()),
                                          c->viewChanBins, c->viewSlice.data(), c->vectx, blockIndex);
            if (gv > 0) c->viewRx->feed(c->viewSlice.data(), gv);
        }
    }

    void feedClientChannels(const cf32* iq, int n) {
        if (!perClientDsp() || n <= 0) return;
        std::vector<std::shared_ptr<ClientDsp>> cs;
        { std::lock_guard<std::mutex> lk(clientMtx);
          cs.reserve(clientDsp.size());
          for (auto& kv : clientDsp) cs.push_back(kv.second); }
        // ★★★ DID THE RADIO MOVE UNDER US? A listener's channel is cut at `vfoHz - rtlCenter`,
        //     worked out once when they tuned — so any later move of the capture leaves every
        //     channel slicing the wrong part of the spectrum, with nothing to correct it until
        //     that listener happens to touch the dial. The deferred dongle move makes this
        //     ordinary rather than exotic: the request is remembered and applied here on the DSP
        //     thread, milliseconds AFTER clientRetune ran against the old centre.
        // ★★ Outside the lock, and a pull rather than a push. Having the mover walk `clientDsp`
        //    would mean taking clientMtx while holding modeMtx, which is the lock order that
        //    deadlocked this server once already. A double comparison per block cannot.
        {
            const double centre = rtlCenter.load();
            for (auto& c : cs)
                if (c && c->rx && c->tunedAtCentre != centre) clientRetune(c.get());
        }
        // ★★★ THE WIDE SPECTRUM MUST RUN WITH NOBODY LISTENING. This used to `return` on an empty
        //     listener list, which is obviously right for the per-listener fan-out and quietly
        //     disastrous for everything the SHARED row feeds: the landing-page spectrogram and the
        //     band-conditions measurement. Both exist to tell somebody who has NOT connected what
        //     this receiver has been hearing — and they only accumulated while somebody WAS
        //     connected. Stuart left the server overnight and woke to an empty spectrogram: "it's
        //     like it never woke up" (2026-08-06). Measured: 48 rows over 16 minutes on a server
        //     up 5h46m — exactly the span since he opened the page.
        //     ★★ The cost is the channelizer's forward FFT, which is the FIXED part of the DSP and
        //        is paid the moment one listener arrives anyway. What is skipped below is the
        //        fan-out, which is the part that scales — so an idle server does the cheap half.
        //     ★ The idle saver is a separate matter and still applies: when capture genuinely
        //       parks there is no IQ at all and this is never called.
        if (!chan_ || chan_->fftSize() != fftSize) chan_.reset(new vibedsp::Channelizer(fftSize));
        // ★ Split the channelizer's own forward FFT from the per-listener work, because they
        //   scale completely differently: the FFT is fixed no matter how many listeners there
        //   are, the fan-out is proportional. Confusing the two is how "per-client cost" ends up
        //   looking enormous with a single listener connected.
        auto fanT0 = std::chrono::steady_clock::now();
        double fanMs = 0, wideMs = 0;
        int blocksThisFeed = 0;
        chan_->feed(iq, n, [&](const cf32* bins, int nbins) {
            (void)nbins;
            blocksThisFeed++;
            const auto f0 = std::chrono::steady_clock::now();
            // ★★★ HAND THE BLOCK OVER AND MOVE ON — no barrier, no waiting.
            //     The DSP thread's only job is the shared forward FFT; every listener's channel
            //     is done on that listener's OWN thread. Nothing a listener does can delay the
            //     radio, and the kernel is free to place them across the cores.
            //     ★ One copy of the block per round, shared by reference: the channelizer
            //       overwrites its buffer on the next block, so the data has to be taken, but it
            //       only has to be taken ONCE however many listeners there are.
            handBlockToListeners(cs, bins, nbins);
            fanMs += std::chrono::duration<double,std::milli>(
                         std::chrono::steady_clock::now() - f0).count();
            // ★★★ THE WIDE WATERFALL COMES OFF THIS SAME FFT — ka9q's whole point.
            //     We were running TWO 32768-point forward transforms of the same samples: one
            //     here for the channels and one inside the shared RxPipeline for the display,
            //     together about 60% of the DSP thread's real-time budget BEFORE a single
            //     listener connected. ka9q-radio runs exactly one and takes everything from it,
            //     which is what lets it carry dozens of channels; so do we now.
            //     ★★ The trade is real and worth knowing: this transform is UNWINDOWED, because
            //        overlap-save fast convolution requires it. A rectangular window leaks more
            //        than the Nuttall the display FFT used, so a strong carrier smears a little
            //        wider. That is inherited from the architecture, not a mistake — it is the
            //        same compromise the receivers we are matching make.
            // ★★★ TIMED SEPARATELY, AND THAT IS THE POINT OF THIS SPLIT. Everything inside
            //     feed() that was not the fan-out used to be reported as "channelizer FFT",
            //     so the wide waterfall's own cost — a log10 over every bin, then the emit —
            //     was being charged to the transform. They scale completely differently: the
            //     FFT is the same work every block, this fires only every `decim`th block and
            //     is proportional to fps. A number that mixes a fixed cost with a bursty one
            //     cannot tell you which of them is making the DSP miss real time.
            const auto w0 = std::chrono::steady_clock::now();
            emitWideFromBins(bins, nbins);
            wideMs += std::chrono::duration<double,std::milli>(
                          std::chrono::steady_clock::now() - w0).count();
        });
        const double totalMs = std::chrono::duration<double,std::milli>(
                                   std::chrono::steady_clock::now() - fanT0).count();
        chanFftMs_ += (totalMs - fanMs - wideMs);
        chanFanMs_ += fanMs;
        chanWideMs_ += wideMs;
        chanClients_ += (double)cs.size();
        chanN_++;
        // ★★ THE OTHER HALF OF THE QUESTION: is the INPUT ragged? The forward FFT is a fixed
        //    cost per BLOCK, not per call, so a driver handing us uneven buffers makes some
        //    calls do two transforms and others none — which looks exactly like a DSP that
        //    randomly costs three times as much. Track the spread of both.
        chanBlocksMin_ = std::min(chanBlocksMin_, blocksThisFeed);
        chanBlocksMax_ = std::max(chanBlocksMax_, blocksThisFeed);
        chanBlocksTot_ += blocksThisFeed;
        chanInMin_ = std::min(chanInMin_, n);
        chanInMax_ = std::max(chanInMax_, n);
    }
    double chanFftMs_ = 0, chanFanMs_ = 0, chanWideMs_ = 0, chanClients_ = 0; int chanN_ = 0;
    int chanBlocksMin_ = INT_MAX, chanBlocksMax_ = 0, chanBlocksTot_ = 0;
    int chanInMin_ = INT_MAX, chanInMax_ = 0;

    // ★ The worker POOL that used to live here is gone: every listener now has its own DSP
    //   thread (see ClientDsp::th), so there is nothing to fan out to and no barrier to wait
    //   on. Two mechanisms for one job is how the slow one gets quietly left in place.

    /** Convert one forward-FFT block into the dB row the display path expects, and hand it to
     *  onSpectrum exactly as the pipeline's own FFT used to. */
    std::vector<float> wideRow_;
    int wideDecim_ = 0;
    void emitWideFromBins(const cf32* bins, int n) {
        // ★ MATCH THE OLD RATE. The pipeline's FFT ran at fftRate*FFT_AVG; this block callback
        //   runs far faster (hop is 3/4 of the FFT, so ~325/s at 8 MSPS). Feeding onSpectrum every
        //   block would quadruple the frame rate and the averaging window with it.
        const double blockRate = sampleRate / (double)(n - n / vibedsp::Channelizer::OVERLAP_DIV);
        int decim = (int)std::lround(blockRate / std::max(1.0, fftRate * FFT_AVG));
        if (decim < 1) decim = 1;
        if (++wideDecim_ < decim) return;
        wideDecim_ = 0;
        if ((int)wideRow_.size() != n) wideRow_.assign(n, -200.0f);
        // ★★ FFTSHIFT ON THE WAY OUT. The channelizer leaves DC at bin 0 (natural FFT order);
        //    onSpectrum has always been handed a SHIFTED row, bin 0 = -fs/2. Getting this wrong
        //    puts the centre of the band at the edge of the screen.
        const int half = n / 2;
        const float scale = 1.0f / ((float)n * (float)n);
        for (int i = 0; i < n; i++) {
            const cf32& c = bins[(i + half) % n];
            const float p = (c.real() * c.real() + c.imag() * c.imag()) * scale;
            wideRow_[i] = 10.0f * std::log10(p + 1e-30f) + kWideCalDb;
        }
        onSpectrum(wideRow_.data(), n);
    }
    /** ★★ Level offset that lines this path up with the windowed FFT it replaces, MEASURED not
     *  guessed (tools/vibeserver-probes/framestats.mjs, before and after).
     *  The old path applied a Nuttall window, whose coherent gain is about 0.36 — so it read a
     *  carrier roughly 8.9 dB LOWER than an unwindowed transform of the same signal. Without this
     *  every S-meter reading, every squelch threshold and the waterfall auto-contrast would shift
     *  by that much on the day the FFTs merged.
     *  ★ It cannot be perfect: a window changes the noise floor and the peaks by DIFFERENT
     *    amounts (coherent gain vs noise-equivalent bandwidth), so this matches the PEAKS, which
     *    is what the S-meter and the squelch actually read. The floor lands ~3 dB high. */
    static constexpr float kWideCalDb = -8.0f;

    /** Copy this block once and post it to every listener's queue. Never blocks. */
    void handBlockToListeners(const std::vector<std::shared_ptr<ClientDsp>>& cs,
                              const cf32* bins, int nbins) {
        // ★ Nothing to hand out: skip the copy entirely. The wide path still runs above (that is
        //   the point of getting here with no listeners), but copying a 32k block for an empty
        //   list every round would be pure waste on an idle server.
        if (cs.empty()) return;
        auto blk = std::make_shared<ClientDsp::Block>();
        blk->bins.assign(bins, bins + nbins);
        blk->index = chan_->blockIndex();     // ★ the phase reference travels WITH the samples
        for (auto& c : cs) {
            std::lock_guard<std::mutex> lk(c->qm);
            // ★★ A LISTENER THAT CANNOT KEEP UP DROPS ITS OWN BLOCKS. Four blocks is ~12 ms of
            //    slack at 8 MSPS — enough to ride out a scheduling hiccup, short enough that a
            //    genuinely stuck listener does not accumulate latency it can never pay back.
            //    Its audio glitches; the radio and everyone else carry on.
            if (c->q.size() >= 4) { c->q.pop_front(); c->dropped.fetch_add(1); }
            c->q.push_back(blk);
            c->qcv.notify_one();
        }
    }

    int dspBlocks_ = 0;
    double dspWideMs_ = 0, dspPerMs_ = 0, dspRealMs_ = 0;

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
            // ★★★ IS THE DSP KEEPING REAL TIME? The only measure that matters under load, and the
            //     one nothing was watching: a probe that counts frames cannot tell "smooth" from
            //     "stuttered, then caught up" — which is exactly the difference between a passing
            //     test and a listener saying the audio broke up. The IQ backlog IS that signal.
            const auto t0 = std::chrono::steady_clock::now();
            const double haveSec = (double)buf.size() / sampleRate;   // real time in this block
            // ★★★ IN SHARED MODE THE SHARED PIPELINE IS NOT RUN AT ALL. Its FFT is replaced by the
            //     channelizer's (emitWideFromBins), and its DEMODULATOR produced audio that NOBODY
            //     listened to — every listener has their own chain now. Running it was paying full
            //     price for a spectrum we already had and audio nobody could hear.
            if (!perClientDsp()) rx.feed(buf.data(), (int)buf.size());
            const auto tMid = std::chrono::steady_clock::now();
            // ★★★ PER-CLIENT DEMOD. The shared `rx` above still produces the WIDE waterfall
            //     everyone sees; this produces each listener's OWN audio from their own slice of
            //     one shared forward FFT. Costs ~0.09% of a core each (measured, see the spike).
            //     ★ Only in shared/locked mode — a personal receiver never builds any of it, so
            //       the phone and Mac paths are byte-for-byte what they were.
            feedClientChannels(buf.data(), (int)buf.size());
            {
                const auto t1 = std::chrono::steady_clock::now();
                const double wideMs = std::chrono::duration<double,std::milli>(tMid - t0).count();
                const double perMs  = std::chrono::duration<double,std::milli>(t1 - tMid).count();
                dspWideMs_ += wideMs; dspPerMs_ += perMs; dspRealMs_ += haveSec * 1000.0;
                if (++dspBlocks_ >= 200) {
                    size_t q; { std::lock_guard<std::mutex> lk(iqMtx); q = iqQueuedSamples; }
                    // ★ >100% means the DSP cannot keep up and the backlog will grow until
                    //   something drops — the audible symptom is everyone stuttering at once.
                    dspLoadPct = (dspWideMs_ + dspPerMs_) / dspRealMs_ * 100.0;
                    LOGI("dsp load: wide %.0f%% + per-client %.0f%% = %.0f%% of real time "
                         "(backlog %.0f ms)",
                         dspWideMs_ / dspRealMs_ * 100.0, dspPerMs_ / dspRealMs_ * 100.0,
                         (dspWideMs_ + dspPerMs_) / dspRealMs_ * 100.0,
                         (double)q / sampleRate * 1000.0);
                    if (chanN_ > 0) {
                        LOGI("  split: forward FFT %.0f%%, wide emit %.0f%%, fan-out %.0f%% "
                             "for %.1f listeners",
                             chanFftMs_ / dspRealMs_ * 100.0, chanWideMs_ / dspRealMs_ * 100.0,
                             chanFanMs_ / dspRealMs_ * 100.0, chanClients_ / chanN_);
                        LOGI("  input: %d..%d samples/call, %d..%d blocks/call, %.2f blocks/call avg",
                             chanInMin_ == INT_MAX ? 0 : chanInMin_, chanInMax_,
                             chanBlocksMin_ == INT_MAX ? 0 : chanBlocksMin_, chanBlocksMax_,
                             (double)chanBlocksTot_ / chanN_);
                    }
                    chanFftMs_ = chanFanMs_ = chanWideMs_ = chanClients_ = 0; chanN_ = 0;
                    chanBlocksMin_ = chanInMin_ = INT_MAX;
                    chanBlocksMax_ = chanInMax_ = chanBlocksTot_ = 0;
                    dspBlocks_ = 0; dspWideMs_ = dspPerMs_ = dspRealMs_ = 0;
                }
            }
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
    /** When the idle park becomes due (nowSecs()), or 0 if no park is pending. Armed when the
     *  LAST listener leaves and cleared the moment one returns — see g_vsIdleGraceSec. */
    std::atomic<double> idleParkDueAt{0.0};
    /// ★★★ IDLE THE DONGLE BY DISCARDING, NOT BY STOPPING IT. See pauseCaptureIdle.
    /// The unix socket other VibeServer processes hand connections to us on.
    /// True for the process that owns the public port and owns no radio.
    bool        frontDoorOnly = false;
    int         handoffFd = -1;
    std::string handoffPath;
    std::thread handoffThread;

    std::atomic<bool> idleDiscard{false};
    /// When we last tried to reopen a dongle whose stream died — see the watchdog.
    double lastReopenAt_ = 0.0;
    void pauseCaptureIdle() {
        // ★★★ A SHARED RECEIVER NEVER IDLE-PARKS. Parking costs the AGC: the RSP's loop only
        //     starts on a TRANSITION (see the sdrpAgcKick note), so every pause/resume cycle makes
        //     it re-converge, and when it sticks the listener has NO GAIN CONTROLS to rescue it —
        //     the operator locked them. On a personal receiver parking is right, because the
        //     dongle is ~80% of the battery; on a shared one it breaks the one thing a listener
        //     cannot fix (Stuart, 2026-08-02: "a stuck agc on a client with no user gain controls
        //     is no good"). Idle power is the operator's problem to accept when they publish.
        if (g_vsLockedCentre.load() > 0.0) {
            LOGI("no listeners — capture STAYS RUNNING (shared receiver: keeps the AGC converged)");
            return;
        }
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
    /** Arm the deferred park. The wait itself happens on the hotplug tick, which already runs
     *  every 2 s — no new thread, and nothing to join on shutdown. */
    /** Bring the gain down to the owner's ceiling for this frequency, if there is one.
     *  ★ Reads the CURRENT gain and only lowers it, so a listener who was already inside the
     *    limit is not disturbed and nothing is silently raised. */
    /** The ceiling last announced to clients, so a change can be sent and nothing else. */
    std::atomic<int> lastSentGainCap{-2};

    void applyGainCapForFreq(double hz) {
        const int cap = LocalSdrShim::gainCapAt(hz);
        // ★★★ TELL THE CLIENTS WHEN THE CEILING CHANGES — including when it goes AWAY. hwinfo is
        //     otherwise sent once, on connect, so a listener who tunes from HF into FM would keep
        //     a slider bounded by the old answer: either refusing gain the owner now allows, or
        //     offering gain that is about to be clamped. Both read as a broken control.
        //     ★ Only on a CHANGE. This runs on every retune, and re-sending the whole hardware
        //       description on each dial movement would be a message per pan.
        if (lastSentGainCap.exchange(cap) != cap)
            for (auto& pr : allSpecPeers()) sendHwInfo(pr.sock);
        if (cap < 0) return;
        if (useSdrplay() && sdrp) {
            // ★★ The cap is a GAIN POSITION; the LNA state counts the other way. See the note in
            //    the rsp_control handler — this is the same conversion and must not drift from it.
            const int n = sdrp->lnaStateCount();
            if (n > 1) {
                const int minState = std::max(0, (n - 1) - cap);
                if (sdrp->currentLnaState() < minState) {
                    LOGI("retune into a capped band — RF gain state %d -> %d",
                         sdrp->currentLnaState(), minState);
                    LocalSdrShim::instance().setLnaState(minState);
                }
            }
            return;
        }
        // RTL: tenths of a dB, and -1 means AUTO — which is the radio deciding, not the listener
        // overriding, so it is left alone.
        const int cur = lastGainTenthDb;
        if (cur >= 0 && cur > cap) {
            LOGI("retune into a capped band — gain %d -> %d", cur, cap);
            LocalSdrShim::instance().setGain(cap);
        }
    }

    /** ★★★ THE GAIN THE OWNER CHOSE, ONCE EVERYBODY HAS GONE.
     *
     *  A cap stops a listener going too far; this stops what they DID becoming the receiver's new
     *  normal. Without it the next person — and the owner — inherit whatever the last listener
     *  happened to leave behind, which is how a receiver ends up quietly overloaded with nobody
     *  having done anything wrong (Stuart, 2026-08-12: "always return to around 19.7").
     *  ★★ AT THE PARK, NOT ON DISCONNECT. The park is already deferred by the grace period exactly
     *     so a page reload is not treated as leaving — resetting on disconnect would undo a
     *     listener's own setting every time they refreshed the page.
     *  ★ In the radio's own units, like everything else here: tenths of a dB on an RTL, an RF gain
     *    POSITION on an RSP. -1 = the owner has not set one, so leave the radio exactly as it is.
     */
    void applyRestGain() {
        const int rest = g_restGain.load();
        if (rest < 0) return;
        if (useSdrplay() && sdrp) {
            const int n = sdrp->lnaStateCount();
            if (n > 1) {
                const int state = std::max(0, std::min(n - 1, (n - 1) - rest));
                LOGI("everybody has left — RF gain back to the owner's resting position %d", rest);
                LocalSdrShim::instance().setLnaState(state);
            }
            return;
        }
        LOGI("everybody has left — gain back to the owner's resting value %d", rest);
        LocalSdrShim::instance().setGain(rest);
    }

    void armIdlePark() {
        // ★ Applied here rather than when the park FIRES: the radio may be released or paused by
        //   then, and a gain written to a stopped device is a gain nobody applied. The grace
        //   period is about not disturbing a reload — and a reload does not care what the gain is
        //   while nobody is listening, because resumeCaptureIdle leaves it exactly as found.
        applyRestGain();
        const double g = g_vsIdleGraceSec.load();
        if (g <= 0.0) {                                      // grace disabled: act at once
            if (g_vsReleaseWhenIdle.load()) LocalSdrShim::instance().releaseRadio();
            else                            pauseCaptureIdle();
            return;
        }
        idleParkDueAt.store(nowSecs() + g);
        LOGI("no listeners — idle park in %.0fs (grace period)", g);
    }
    void resumeCaptureIdle() {
        // ★ A listener is back, so any pending park is off. This runs EARLY in the connect path,
        //   which is exactly what makes the grace period work for a page reload: the park was
        //   only ever scheduled, so it is cancelled before it can happen.
        if (idleParkDueAt.exchange(0.0) > 0.0)
            LOGI("listener returned within the grace period — idle park cancelled");
        // ★★ TAKE THE RADIO BACK FIRST, because everything below assumes a device. If another
        //    program has it we carry on WITHOUT one rather than failing the connection: the
        //    listener gets the server, the band plan and a clear reason, which is far better than
        //    a socket that refuses to open or a waterfall that is simply blank.
        if (radioReleased.load()) {
            std::string err;
            if (!LocalSdrShim::instance().reacquireRadio(err))
                LOGI("listener arrived but the radio is not ours to take back — %s", err.c_str());
        }
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
        // ★ THIS listener's decoder, like the basic RDS path — the Advanced panel is the one that
        //   most obviously must not show somebody else's station: it is all constellation, phase
        //   and block-error figures for a signal the reader is judging by ear at the same time.
        auto c_ = dspFor(sock);
        auto src_ = rdsSourceFor(c_);
        RdsState& R = src_ ? src_->rdsS : (c_ ? c_->rdsS : rdsS);
        int pty, tp, ta, ms, di, ctMin, ctOff, gTot, afSeen;
        int ptyR, tpR, taR, msR, diR;
        int lang, pinD, pinH, pinM; float phase, phaseCoh, pilotDev, rdsDev_, phaseDrift;
        int berNow;   // ★ block error rate, ALSO here: the phase verdict needs it
        std::string rtpT, rtpA, lps, ptyn;
        std::vector<vibedsp::RdsDecoder::Eon> eon;
        std::vector<vibedsp::RdsDecoder::Oda> oda;
        std::vector<int> af, grp, afAll; std::vector<unsigned char> afAllOk;
        std::vector<float> pts, mpx;
        { std::lock_guard<std::mutex> lk(R.rdsMtx);
          pty = R.rdsPty; tp = R.rdsTp; ta = R.rdsTa; ms = R.rdsMs; di = R.rdsDi;
          ptyR = R.rdsPtyRaw; tpR = R.rdsTpRaw; taR = R.rdsTaRaw; msR = R.rdsMsRaw; diR = R.rdsDiRaw;
          ctMin = R.rdsCtMin; ctOff = R.rdsCtOff; gTot = R.rdsGrpTotal;
          af = R.rdsAf; afAll = R.rdsAfAll; afAllOk = R.rdsAfAllOk; grp = R.rdsGrp; pts = R.rdsConst; mpx = R.rdsMpx; afSeen = R.rdsAfSeen;
          rtpT = R.rdsRtpTitle; rtpA = R.rdsRtpArtist; lps = R.rdsLongPs; ptyn = R.rdsPtyn;
          lang = R.rdsLang; pinD = R.rdsPinDay; pinH = R.rdsPinHour; pinM = R.rdsPinMin;
          eon = R.rdsEon; oda = R.rdsOda; phase = R.rdsPhase; phaseCoh = R.rdsPhaseCoh;
          phaseDrift = R.rdsPhaseDrift;
          pilotDev = R.rdsPilotDev; rdsDev_ = R.rdsDev; berNow = R.rdsBer; }
        // ★ The pipeline whose figures these are — chosen exactly as RdsState was above, so the
        //   numbers describe the SAME signal as the constellation beside them. A shared radio has
        //   one pipeline per client; a single-user radio has only the Impl's own.
        const vibedsp::RxPipeline* P_ = (src_ && src_->rx) ? src_->rx.get()
                                      : ((c_ && c_->rx)    ? c_->rx.get() : &rx);
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
                      // ★★★ "rdsDev", NOT "R.rdsDev". A stray "R." prefix — almost certainly a
                      //     half-finished edit from when these were read off a struct called R —
                      //     meant the client looked up msg.rdsDev, found undefined, and drew a
                      //     dash. FOREVER: the RDS DEV field has never once populated (Stuart,
                      //     2026-08-11, wanting it for Hans, who measures against a Pira).
                      //     ★★ It is invisible from either side alone. The server "sends the
                      //        deviation" and the client "reads the deviation"; only the WIRE
                      //        shows they are not the same word — and every neighbouring field
                      //        (pilotDev, phaseCoh, ber) is spelled correctly, so nothing looked
                      //        odd in review. See [[wire_value_derived_both_ends]].
                      + ",\"rdsDev\":" + std::to_string(rdsDev_)
                      + ",\"ber\":" + std::to_string(berNow)
                      // ★★★ THE WEAK-SIGNAL FIGURES, KEPT ON PURPOSE — not debug scaffolding.
                      //     Stuart: "the FM-DX crowd would appreciate them anyway and that would
                      //     be the place for them". They belong beside pilot deviation and block
                      //     error rate because they are the same KIND of thing: measurements a
                      //     listener judges a marginal signal by, at the same time as judging it
                      //     by ear.
                      //     · mpxSnr    — pilot against the transmitted-silence gap at 15-19 kHz.
                      //                   NOT a textbook SNR (the measuring filter's own leakage
                      //                   caps it near 34 dB); it is a relative figure of merit.
                      //     · multipath — envelope AM depth. An FM carrier leaves the transmitter
                      //                   at CONSTANT amplitude, so anything here was done by the
                      //                   channel: this reads a REFLECTION, not weakness, and the
                      //                   two want opposite treatments.
                      //     · hiCutLmr  — where high-blend has rolled the stereo difference off.
                      //     · hiCutAud  — where the audio high-cut is sitting.
                      //   ★★ The last two exist so "why is this not in full stereo?" and "why does
                      //      this sound dull?" have an ANSWER rather than a suspicion — the same
                      //      rule as every other sticky control reporting its state.
                      + ",\"mpxSnr\":"    + std::to_string(P_ ? P_->blendSnrDb() : 0.0f)
                      // ★ The CORRECTED figure — the noise contribution has been measured and
                      //   subtracted — plus whether it means anything at this S/N. Sending the
                      //   raw number would repeat the mistake that labelled a 6 dB signal's noise
                      //   as "severe multipath".
                      + ",\"multipath\":" + std::to_string(P_ ? P_->multipathDepth() : 0.0f)
                      + ",\"multipathOk\":" + std::string((P_ && P_->multipathValid()) ? "1" : "0")
                      // ★ Is there a pilot to measure ANY of this against? Without one, mpxSnr and
                      //   multipath are both ratios with a collapsed denominator.
                      + ",\"snrOk\":" + std::string((P_ && P_->snrValid()) ? "1" : "0")
                      + ",\"hiCutLmr\":"  + std::to_string(P_ ? P_->lmrHiCutHz() : 0.0f)
                      + ",\"hiCutAud\":"  + std::to_string(P_ ? P_->audioHiCutHz() : 0.0f)
                      // ★ WOULD A NARROWER IF HELP THIS SIGNAL? Measured live on a shadow copy —
                      //   see RxPipeline's shadow receiver. Shown BEFORE anything acts on it, so
                      //   the policy that will eventually drive it can be judged rather than
                      //   trusted (Stuart: "add the benefit measurement in to the advanced rds").
                      + ",\"ifGain\":" + std::to_string(P_ ? P_->ifGainDb() : 0.0f)
                      + ",\"ifCand\":" + std::to_string(P_ ? P_->ifCandidateHz() : 0.0)
                      // ★ WHICH STATE WE ARE IN — without it `ifGain` is ambiguous, because the
                      //   shadow always evaluates THE OTHER OPTION: narrow while we are wide, wide
                      //   while we are narrowed. The sign flips on engagement, and a reader with
                      //   no state would misread "-11 dB" as bad news when it means the opposite.
                      + ",\"ifBw\":" + std::to_string(P_ ? P_->ifBandwidth() : 0.0)
                      // ★ CEQ, with its OWN SCORE. `ceqAfter` is the multipath depth measured on
                      //   the equaliser's output, against `multipath` measured on what arrived —
                      //   so the panel shows what it achieved rather than merely that it ran.
                      + ",\"ceqOn\":" + std::string((P_ && P_->ceqEngaged()) ? "1" : "0")
                      + ",\"ceqAfter\":" + std::to_string(P_ ? P_->multipathAfterCeq() : 0.0f)
                      + ",\"ceqWhy\":" + std::to_string(P_ ? P_->ceqWhy() : 3)
                      // ★ The blanker's RATE is the diagnostic that matters: it tells an owner
                      //   whether they have an impulse-noise problem at all, which is otherwise
                      //   pure guesswork ("is that crackle me, or the station?").
                      + ",\"nbRate\":" + std::to_string(P_ ? P_->noiseBlankRate() : 0.0f)
                      // ★★ THE AF LIST WITH A TICK EACH, as the FM-DX Webserver shows it: a bare
                      //    "3 of 7" says the link is damaging entries but not WHICH frequencies,
                      //    and on a noisy station the unconfirmed ones are usually phantoms
                      //    manufactured by block errors rather than real alternatives.
                      + ",\"afAll\":[";
        for (size_t i = 0; i < afAll.size(); ++i) {
            if (i) j += ',';
            j += "[" + std::to_string(afAll[i]) + ","
               + std::string((i < afAllOk.size() && afAllOk[i]) ? "1" : "0") + "]";
        }
        j += "],\"grp\":[";
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
    /** ★★★ RE-LOCK AN IDLE ADMIN, WITHOUT ENDING THEIR SESSION.
     *
     *  Stuart, 2026-08-06: *"an admin may have opened a session to make some tweaks and left a
     *  decode running and forgotten that they were logged in as admin, which then means anybody
     *  else could then interact with the open tab to meddle."* The threat is not a guessed
     *  password — it is a browser tab in a shack or an office, hours after the last click, still
     *  holding the right to change the gain on a live receiver.
     *
     *  ★★★ THE WHOLE DESIGN IS IN WHAT IT DOES *NOT* DO. It does not disconnect, mute, stop the
     *  decoder or surrender the slot. Leaving a session running unattended is a NORMAL, INTENDED
     *  use of this software — a long listen, an FT8 run, a recording — so ending it would punish
     *  the ordinary case in order to defend against the careless one. Only the ability to CHANGE
     *  anything is withdrawn, and the menu's existing password box gives it straight back.
     *
     *  ★★ "INTERACTION" MEANS A CONTROL MESSAGE, NOT TRAFFIC. Audio and spectrum frames flow
     *  continuously whether or not a human is in the room, so any measure built on them would
     *  never fire — the session that most needs re-locking is precisely the one still streaming
     *  happily to an empty chair. `lastAdminTouch` is stamped where the client sends a COMMAND. */
    void enforceAdminIdle() {
        const int idleMin = g_vsAdminIdleMin.load();
        if (idleMin <= 0) return;                        // owner switched it off
        // ★★ EVERY LISTENER WHO HOLDS IT, not one flag for the radio. Each one's idle clock runs
        //    from ITS OWN last command, which is the only reading that means anything: one owner
        //    working the controls must not keep another's unlock alive, and must not be re-locked
        //    by somebody else's inactivity either.
        {
            std::lock_guard<std::mutex> lk(clientMtx);
            for (auto& kv : clientDsp) {
                auto& c = kv.second;
                if (!c || !c->adminOk.load()) continue;
                const double lt = c->lastAdminTouch.load();
                if (lt <= 0 || Impl::nowSecs() - lt < (double)idleMin * 60.0) continue;
                c->adminOk.store(false);
                LOGI("admin re-locked after %d min idle for one listener", idleMin);
            }
        }
        if (!adminOk.load()) return;                     // nothing radio-wide to re-lock
        const double last = lastAdminTouch.load();
        if (last <= 0) return;
        if (Impl::nowSecs() - last < (double)idleMin * 60.0) return;

        adminOk.store(false);
        LOGI("admin controls re-locked after %d min idle (session left running)", idleMin);
        // ★ TELL THE CLIENT WHY. A control that silently stops working reads as a bug, and this
        //   one stops working a long time after anybody touched it — so without a message the
        //   owner returns to a receiver that has apparently broken itself. `relocked` is what
        //   raises the pill; `ok:false` is what greys the controls, through the path the client
        //   already has for a refusal.
        std::shared_ptr<net::Socket> sc;
        { std::lock_guard<std::mutex> lk(clientMtx); sc = specClient; }
        if (sc && sc->isOpen())
            sendText(sc, "{\"type\":\"admin\",\"ok\":false,\"relocked\":true,\"idleMin\":"
                       + std::to_string(idleMin) + "}");
    }

    void enforceSessionLimit() {
        const int limitMin = g_vsSessionLimitMin.load();
        if (limitMin <= 0) return;                       // unlimited: the default

        std::shared_ptr<net::Socket> spec, aud;
        std::string addr; double since; int warned;
        { std::lock_guard<std::mutex> lk(clientMtx);
          if (occupantSession.empty() || occupantSince <= 0) return;
          spec = specClient; aud = audioClient;
          addr = occupantAddr; since = occupantSince; warned = occupantWarned; }
        if (addr.empty() || isLoopback(addr)) return;    // the host's own listening
        // ★★★ THE EXEMPTION IS THE OCCUPANT'S OWN, and this read the radio-wide flag while
        //     occupantSecsLeft() had just been made per-listener — so the countdown would say "no
        //     limit" to an admin and this would disconnect them anyway. A display that disagrees
        //     with the enforcement is worse than either being wrong on its own: it tells the owner
        //     they are safe right up until they are cut off.
        // ★ Outside the lock above: adminNow() takes clientMtx itself.
        if (adminNow(spec)) return;                      // the owner is exempt

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
        if (spec && spec->isOpen()) { sendWs(spec, 0x1, (const uint8_t*)m.data(), m.size()); spec->closeAfterFlush(); }
        if (aud  && aud->isOpen())  { aud->close(); }
        LocalSdrShim::noteConnectionClosed(addr, "", "timeout");
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

                // ── Deferred idle park (see armIdlePark / g_vsIdleGraceSec) ──────────────
                // ★ THE PARK IS RE-JUSTIFIED HERE, NOT MERELY REMEMBERED. Anything could have
                //   happened during the grace period, so the decision is taken against the state
                //   NOW: a listener may have returned, and the AGC settle may still be running.
                // ★★★ ARM IT HERE TOO, NOT ONLY ON A DISCONNECT. armIdlePark() was reached only
                //     from the WebSocket close path, so a radio that came up with nobody listening
                //     never armed anything and streamed IQ for ever: the Airspy's IQ light stayed
                //     on with no listeners, and the RTL never let go however short its grace was
                //     set (Stuart, 2026-08-08 — "seems like a waste of power", and the OWRX probe
                //     confirmed it: usb_claim_interface error -6). A server that boots idle is the
                //     NORMAL state of an unattended receiver, and it was the one state that never
                //     parked.
                // ★★ The spectrogram radio is exempt from PAUSING — its whole job is to keep
                //    listening when nobody else is — but not from releasing, since letting the
                //    device go already means giving the spectrogram up.
                // ★★ THE SPECTROGRAM RADIO IS EXEMPT. Its whole job is to keep listening when
                //    nobody else is — a 24-hour picture of the band cannot be drawn by a receiver
                //    that sleeps through it. A duty cycle was tried and taken back out: waking,
                //    waiting for the RSP's AGC to settle and sampling is more moving parts than a
                //    working picture is worth risking (Stuart, 2026-08-08: "I'd rather not break
                //    something that works"). The COST is real and is now stated on the toggle that
                //    chooses it, which is where an owner can weigh it.
                if (idleParkDueAt.load() == 0.0 && !captureIdle.load() && !radioReleased.load()
                    && !(g_vsProvidesSpectrogram.load() && !g_vsReleaseWhenIdle.load())) {
                    bool empty;
                    { std::lock_guard<std::mutex> lk(clientMtx);
                      empty = (!specClient  || !specClient->isOpen())
                           && (!audioClient || !audioClient->isOpen())
                           // ★ AND THE EXTRA SPECTRUM LISTENERS. This asked only about the primary
                           //   pair, so on a multi-listener radio a second viewer was invisible to
                           //   the idle decision entirely — it could park or release a radio that
                           //   somebody was still watching.
                           && std::none_of(specExtra.begin(), specExtra.end(),
                                           [](const std::shared_ptr<net::Socket>& e){
                                               return e && e->isOpen(); }); }
                    if (empty) armIdlePark();
                }

                if (const double due = idleParkDueAt.load(); due > 0.0 && nowSecs() >= due) {
                    bool empty;
                    { std::lock_guard<std::mutex> lk(clientMtx);
                      empty = (!specClient  || !specClient->isOpen())
                           && (!audioClient || !audioClient->isOpen()); }
                    // ★★ NEVER PARK MID-SETTLE. Parking costs the AGC its convergence, so cutting
                    //    the kick short would hand the next listener exactly the stuck AGC this
                    //    whole sequence exists to prevent. Wait for it; it takes ~6 s.
                    const bool settling = useSdrplay() && sdrpAgcWanted && sdrpAgcKick < 6;
                    if (!empty) { idleParkDueAt.store(0.0); }
                    else if (settling) { /* hold the deadline open and re-check next tick */ }
                    else if (g_vsReleaseWhenIdle.load()) {
                        // ★★★ RELEASE, NOT PARK — and note this bypasses the shared-receiver rule
                        //     that pauseCaptureIdle enforces. Parking is skipped on a locked
                        //     receiver to keep the RSP's AGC converged; here the operator has
                        //     explicitly asked for the device to be let go, and keeping it for the
                        //     AGC's sake would defeat the entire feature. The re-converge on the
                        //     next listener is the price, and the setup page says so.
                        idleParkDueAt.store(0.0);
                        LocalSdrShim::instance().releaseRadio();
                    }
                    else { idleParkDueAt.store(0.0); pauseCaptureIdle(); }
                }

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
                // ★ And do not "recover" a radio we have deliberately lent out: reopening it would
                //   take it back from the program we just handed it to.
                const bool recoverable = silent && !captureIdle.load() && !radioReleased.load() &&
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

                // ★★★ SILENCE WE ASKED FOR IS NOT A FAULT. This tested only for silence, so the
                //     moment the radio was deliberately RELEASED — handed to another program, which
                //     is a feature — the watchdog declared the dongle unplugged, set deviceLost and
                //     captureDown, and repeated it every three seconds for ever.
                // ★★★ WITH `idleGrace: 0` THAT HAPPENS AT STARTUP. Nobody is listening one second
                //     after launch, so an instant release fires immediately, and Saber's server
                //     spent its whole life reporting a healthy dongle as unplugged (2026-08-09).
                //     Worse than the noise: deviceLost then poisons the retake path, which is where
                //     the storm of usb_claim_interface -6 came from.
                // ★★ The parked case is the same argument. The Airspy path already dodged this by
                //    reading the SOURCE's last-received time (see above) — the dongle had no such
                //    guard, so an idle-parked dongle read as an unplugged one too.
                // ★ `lsusb` in Saber's log lists the device throughout, which is the tell: the
                //   hardware was never going anywhere.
                const bool expectingIq = !radioReleased.load() && !captureIdle.load();
                // ★★★ EVERY VISITOR LOOKS LIKE LOCALHOST? THEN SOMETHING IS PROXYING AND WE DO NOT KNOW.
        //     A tunnel or a reverse proxy connects from 127.0.0.1, and loopback is EXEMPT from the
        //     session limit — so putting a public receiver behind one silently turns the limit off
        //     for everybody, and fills the ban list, the connection log and the country flags with
        //     127.0.0.1. Nothing looks wrong; the owner just quietly has no limits.
        // ★★★ FOUND THE HARD WAY: the demo went behind a Cloudflare tunnel and the countdown
        //     vanished. The countdown was the only visible symptom of a receiver that had stopped
        //     enforcing anything at all (2026-08-09).
        // ★ Said ONCE, and only when the evidence is unambiguous: several distinct sessions, all
        //   from loopback, with no trusted proxy configured. A single local test connection must
        //   not nag somebody running it on their own desk.
        {
            static bool warned = false;
            if (!warned && g_vsLoopbackSessions.load() >= 3 && !g_vsHaveTrustedProxies.load()) {
                warned = true;
                LOGE("every listener so far has come from 127.0.0.1 — if this server is behind a "
                     "proxy or a tunnel, set Trusted proxies on the Server tab. Until you do, the "
                     "session limit does not apply to anyone and the connection log records no "
                     "real addresses.");
            }
        }

        if (silent && expectingIq && !deviceLost.load()) {
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
                // ★★★ DO NOT PROBE A RADIO WE ARE STILL HOLDING. On an SDRplay, deviceCount()
                // calls sdrplay_api_Open(), GetDevices and then api_Close() — and this process
                // already has a device SELECTED and streaming. That is at best a lie (our own
                // device is in use, so it can report none) and at worst disturbs the live session,
                // because Close() tears down this process's API handle. The honest signal for "is
                // it back" on a source we still hold is IQ ARRIVING, which the branch above now
                // acts on; probe only when there is no source object left to ask.
                const bool back = useAirspyHf() ? (ahf  ? true : vibe::AirspyHfSource::deviceCount() > 0)
                                : useSdrplay()  ? (sdrp ? true : vibe::SdrplaySource::deviceCount() > 0)
                                                : (findOurDevice() >= 0);
                if (back == deviceLost.load()) {      // state changed
                    deviceLost.store(!back);
                    notifyDeviceState();
                }

                // ★★★ AND NOW WE CAN ACTUALLY REOPEN IT. The note above says why this used to be
                //     impossible: `dev` had no lock, so closing it here raced every rtlsdr_set_gain
                //     and tuneHw on the control threads and crashed the server on replug. Those
                //     call sites now take the same recursive lock the RSP and Airspy setters
                //     always did (VIBE_HW_LOCK), which is the refactor that note was waiting for.
                //
                // ★★ WHY IT MATTERS BEYOND UNPLUGGING. An RTL stream can be knocked over by
                //    something else entirely: with an Airspy HF+ STREAMING on this machine, the
                //    dongle's transfers fail ("cb transfer status: 1") within seconds of starting,
                //    and the server then sat there logging "restarting" while restarting nothing.
                //    Measured 2026-08-08 — any two of three radios ran fine, all three did not,
                //    and it was NOT bandwidth (the dongle survives beside an RSP at 8 MSPS, and
                //    still fails beside one at 2 MSPS). Whatever disturbs it, recovering is ours.
                //
                // ★ DONGLES ONLY. An RSP or an Airspy that has stopped is handled by its own
                //   source object; reopenDevice() is librtlsdr all the way down.
                if (back && !useTcp() && !useSpy() && !useSdrplay() && !useAirspyHf()
                         && !radioReleased.load() && !captureIdle.load()) {
                    // ★ Backed off, not hammered. A dongle that cannot hold a stream would
                    //   otherwise be reopened every two seconds for ever, and each attempt is USB
                    //   traffic that makes the contention it is recovering from slightly worse.
                    const double now = nowSecs();
                    if (now - lastReopenAt_ >= 5.0) {
                        lastReopenAt_ = now;
                        // ★ modeMtx directly: VIBE_HW_LOCK is written for LocalSdrShim methods,
                        //   which reach it through `p`. We ARE the Impl.
                        // ★★ Joining the capture thread under this lock is safe, and checked: the
                        //    capture callback takes iqMtx only and never modeMtx, so it cannot be
                        //    waiting on what we hold. That is the difference between this and
                        //    stopDspThread(), which joins a thread that DOES take modeMtx.
                        std::lock_guard<std::recursive_mutex> hw(modeMtx);
                        if (reopenDevice()) {
                            captureDown.store(false);
                            LOGI("RTL-SDR capture recovered");
                        }
                    }
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
    std::atomic<bool>   weakProcOn{true};   // Impl::weakProcOn — replayed onto a fresh Impl
    std::atomic<bool>   imsOn{true};        // Impl::imsOn
    std::atomic<bool>   ceqOn{true};        // Impl::ceqOn
    std::atomic<bool>   nbOn{true};         // Impl::nbOn
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
    // ★★ THE RSP'S BIAS-T HAD NO ENTRY HERE AT ALL, so unlike the RTL's it was never replayed
    //    onto a fresh Impl and never reportable to a client — the same omission, on the same
    //    control, that 2026-08-08 fixed for the DONGLE ("I had enabled the bias-t and was using
    //    it until you restarted it"). The RTL half was fixed and the RSP half was not looked at.
    std::atomic<int>  rspBiasT{-1};      // tri-state: -1 unset, 0 off, 1 on
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

// ★ Forward-declared up by the hwinfo builder, which reports this state to the client.
static int   vsDesiredRfNotch()    { return g_dsp.rspRfNotch.load(); }
static int   vsDesiredDabNotch()   { return g_dsp.rspDabNotch.load(); }
static float vsDesiredNrStrength() { return g_dsp.nrStrength.load(); }
static int   vsDesiredRspBiasT()   { return g_dsp.rspBiasT.load(); }

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
        if (g_dsp.rspBiasT.load()    >= 0)    impl->sdrp->setBiasT(g_dsp.rspBiasT.load() != 0);
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
void mdnsStart(const std::string& host, const std::string& ipv4,
               uint16_t servicePort = 0, bool pinRequired = false);
void mdnsStop();
std::string mdnsHost();

void LocalSdrShim::startMdns(const std::string& host, const std::string& ipv4) {
    mdnsStart(host, ipv4);          // hostname only — Android's NsdManager publishes the service
}
/** ★ Publish the SERVICE as well. Linux has no NsdManager and no NetService, so without this a
 *  daemon resolves by name and is still invisible to every client that discovers by browsing. */
void LocalSdrShim::startMdnsService(const std::string& host, const std::string& ipv4,
                                    int port, bool pinRequired) {
    mdnsStart(host, ipv4, (uint16_t)port, pinRequired);
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
void LocalSdrShim::setGainLimits(const std::string& csv) {
    std::lock_guard<std::mutex> lk(g_gainLimMtx);
    g_gainLimits = vibebands::parseGainList(csv);
    LOGI("gain limits: %zu rule(s) from \"%s\"", g_gainLimits.size(), csv.c_str());
}
void LocalSdrShim::setRestGain(int gain) {
    g_restGain.store(gain);
    LOGI("resting gain: %d", gain);
}
void LocalSdrShim::setAgcLock(bool on) {
    g_agcLock.store(on);
    LOGI("AGC lock: %s", on ? "ON — listeners may not turn it off" : "off");
}
bool LocalSdrShim::agcLocked() { return g_agcLock.load(); }

int LocalSdrShim::gainCapAt(double hz) {
    std::lock_guard<std::mutex> lk(g_gainLimMtx);
    if (g_gainLimits.empty()) return -1;
    return vibebands::gainCapAt(g_gainLimits, hz);
}

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
int LocalSdrShim::listenerCount() const { return p ? p->specListenerCount() : 0; }

double LocalSdrShim::captureSpanHz() const { return p ? p->sampleRate : 0.0; }

double LocalSdrShim::listenFrequency() const {
    if (!p) return 0.0;
    const double vfo = p->audioFreq.load();
    return vfo > 0 ? vfo : p->rtlCenter.load();
}

void LocalSdrShim::setSpectrogramPath(const std::string& path) {
    if (!p) return;
    p->spectroSetPath(path);
    p->spectroLoad();
}
void LocalSdrShim::saveSpectrogram() { if (p) p->spectroSave(); }
void LocalSdrShim::saveSpectrogramIfDue() {
    if (p && p->needSpectroSave.exchange(false)) p->spectroSave();
}

int LocalSdrShim::waitingCount() const {
    if (!p) return 0;
    std::lock_guard<std::mutex> lk(p->clientMtx);
    return (int)p->waitQueue.size();
}

// ── ★★★ THE ADMIN API's back end ──────────────────────────────────────────────────────────────

void LocalSdrShim::setBanListPath(const std::string& path) { g_vsBans.setPath(path); }
void LocalSdrShim::setNoticePath(const std::string& path) { g_vsNotice.setPath(path); }
std::string LocalSdrShim::noticeText() { return g_vsNotice.current(); }
bool LocalSdrShim::setNotice(const std::string& text, int minutes, std::string& err) {
    return g_vsNotice.set(text, minutes, err);
}
void LocalSdrShim::setConnLogPath(const std::string& path) {
    // ★ Teach the log how to resolve a country and a network. It cannot call geoip/asndb itself —
    //   they live out here — and without these the history could only ever replay the SNAPSHOT
    //   taken when the connection opened, which is how a laptop stayed logged in the US after the
    //   database knew better.
    vibeadmin::ccResolver()  = [](const std::string& ip) { return vsCountry(ip); };
    vibeadmin::netResolver() = [](const std::string& ip) { return vsAsnLabel(ip); };
    g_vsConnLog.setPath(path);
}
void LocalSdrShim::saveConnLogIfDue() { g_vsConnLog.saveIfDue(); }
void LocalSdrShim::setMaintenanceActions(const std::string& csv) {
    std::lock_guard<std::mutex> lk(g_vsMaintMtx);
    g_vsMaintActions = csv;
    LOGI("maintenance actions offered: %s", csv.empty() ? "(none)" : csv.c_str());
}
void LocalSdrShim::setUpdateSchedule(int srvHour, int srvDay, int allHour, int allDay) {
    g_vsUpdSrvHour.store(srvHour); g_vsUpdSrvDay.store(srvDay);
    g_vsUpdAllHour.store(allHour); g_vsUpdAllDay.store(allDay);
}
void LocalSdrShim::setPublicSharing(bool on) {
    g_vsPublicSharing.store(on);
    LOGI("sharing: %s", on ? "public — full admin tools" : "local — simple admin");
}
void LocalSdrShim::setAdminIdleMinutes(int minutes) {
    g_vsAdminIdleMin.store(minutes >= 0 ? minutes : 0);
    LOGI("admin idle re-lock: %d min", g_vsAdminIdleMin.load());
}
bool LocalSdrShim::isBanned(const std::string& ip, std::string* reason) {
    return g_vsBans.banned(ip, reason);
}
void LocalSdrShim::noteConnectionOpened(const std::string& ip, const std::string& session,
                                        const std::string& agent, const std::string& cc) {
    g_vsConnLog.open(ip, session, agent, cc);
}
void LocalSdrShim::noteConnectionClosed(const std::string& ip, const std::string& session,
                                        const char* reason, uint64_t bytes) {
    g_vsConnLog.close(ip, session, reason, bytes);
}

/** ★★ EVERYTHING THE MONITOR PAGE DRAWS, IN ONE REQUEST. A page that polls five endpoints once a
 *  second is five times the wake-ups on a Pi that is also running the DSP, and its panels can
 *  disagree with each other — the listener count from one fetch beside the bandwidth from
 *  another, taken 300 ms apart. One snapshot is one consistent moment. */
std::string LocalSdrShim::adminStatusJson() {
    const vibeadmin::SysStats sys = vibeadmin::readSys();
    std::string j = "{\"sys\":" + vibeadmin::sysJson(sys);

    j += ",\"listeners\":" + std::to_string(listenerCount())
       + ",\"maxUsers\":"  + std::to_string(g_vsMaxUsers.load())
       + ",\"waiting\":"   + std::to_string(waitingCount());

    // ★ The radio, as the hardware endpoint reports it — an owner looking at a monitor page
    //   wants "is the dongle still there" answered HERE, not on another screen.
    {
        const bool lost = p && p->deviceLost.load();
        j += std::string(",\"radio\":{\"present\":") + (lost ? "false" : "true")
           + ",\"driver\":\"" + (lost ? "none" : isSdrplay() ? "sdrplay"
                                              : isAirspyHf() ? "airspyhf" : "rtl") + "\"";
        j += ",\"centreHz\":" + std::to_string((long long)(g_vsLockedCentre.load() > 0
                                    ? g_vsLockedCentre.load() : (p ? p->rtlCenter.load() : 0)));
        j += ",\"spanHz\":" + std::to_string((long long)captureSpanHz());
        // ★★★ WHAT THE FRONT END IS ACTUALLY DOING, on the page an owner opens when something is
        //     wrong. This telemetry has always existed — it goes to every listener as `rspstat` —
        //     but it never reached the admin API, so diagnosing a gain problem meant reading the
        //     STARTUP LOG and inferring. I did exactly that and got it wrong: the AGC's settling
        //     ritual (`ifgr 59, sysGain 2.8 dB`) is not the operating point it hands over to
        //     (Stuart, 2026-08-06 — his receiver was sitting at a perfectly healthy 18.8 dB).
        //     ★★ Same rule as the CPU governor and the mDNS name: show what IS, never what was
        //        asked for or what it was a minute ago.
        if (p && p->useSdrplay() && p->sdrp) {
            char b[192];
            snprintf(b, sizeof b,
                     ",\"frontEnd\":{\"sysGainDb\":%.1f,\"lna\":%d,\"lnaStates\":%d,"
                     "\"ifGrDb\":%d,\"overload\":%s,\"ifAgc\":%s}",
                     p->sdrp->systemGainDb(), p->sdrp->currentLnaState(),
                     p->sdrp->lnaStateCount(), p->sdrp->currentIfGr(),
                     p->sdrp->overloaded() ? "true" : "false",
                     g_vsSavedIfAgc.load() == 0 ? "false" : "true");
            j += b;
        }
        j += ",\"lockedCentre\":" + std::to_string((long long)g_vsLockedCentre.load()) + "}";
    }

    // ★★ BYTES SENT, AS A RATE. The raw counters are monotonic since start, which answers
    //    "how much have we ever sent" — a question nobody asks. What an owner wants is "am I
    //    saturating my uplink right now", so the delta since the last call is what is reported.
    //    ★ Kept here rather than in the page: two browsers polling at different intervals would
    //      each compute a different rate from the same counters, and both would be wrong.
    if (p) {
        static std::mutex        rateMtx;
        static uint64_t          lastBytes = 0;
        static double            lastAt = 0;
        const uint64_t total = p->vsSpecBytes.load(std::memory_order_relaxed)
                             + p->vsAudioBytes.load(std::memory_order_relaxed);
        const double now = Impl::nowSecs();
        double kbps = 0;
        { std::lock_guard<std::mutex> lk(rateMtx);
          if (lastAt > 0 && now > lastAt && total >= lastBytes)
              kbps = (double)(total - lastBytes) * 8.0 / 1000.0 / (now - lastAt);
          lastBytes = total; lastAt = now; }
        char b[96];
        snprintf(b, sizeof b, ",\"txBytes\":%llu,\"txKbps\":%.1f",
                 (unsigned long long)total, kbps);
        j += b;
        // Feed the history ring from the same snapshot, so the graph and the readouts can
        // never disagree about what a moment looked like.
        vibeadmin::HistSample hs;
        hs.atEpoch   = vibeadmin::nowEpoch();
        hs.load1     = (float)sys.load1;
        hs.tempC     = (float)sys.tempC;
        hs.listeners = (uint16_t)listenerCount();
        hs.kbps      = (uint32_t)kbps;
        hs.mhz       = (uint16_t)(sys.cpuKHz / 1000);
        g_vsHistory.push(hs);
    }

    j += ",\"bans\":" + g_vsBans.json();
    j += ",\"uniqueDay\":" + std::to_string(g_vsConnLog.uniqueSince(24 * 3600));
    // ★ Where listeners come from, over the last day. Empty when the country data has not been
    //   downloaded yet — the page must draw that as absence, not as "nowhere".
    j += ",\"countries\":" + g_vsConnLog.topCountriesJson(24 * 3600);
    j += ",\"uniqueHour\":" + std::to_string(g_vsConnLog.uniqueSince(3600));
    // ★ Decoder health: attached, and actually receiving samples. See decoderFedSamples.
    if (p) {
        j += std::string(",\"decoderAttached\":") + (p->decoderAttached.load() ? "true" : "false")
           + ",\"decoderFedSamples\":"
           + std::to_string((unsigned long long)p->decoderFedSamples.load());
        // ★★★ RESYNCS AND LEVEL. See FskDecoder::resyncs() — a resync discards the decoder's
        //     whole state, so a counter climbing while text is on the air IS the "clean for a
        //     bit then slips out" symptom, measured rather than described. `audioLevel` beside
        //     `audioThreshold` says whether the signal is simply too quiet for the decoder,
        //     which is a completely different fix from a bad signal.
        // ★★★ WHICH decoder is actually running. Exposed because "attached" was true while
        //     NOTHING had been constructed — WEFAX fell through its dispatch for months and the
        //     only outward sign was an image that never arrived. A name here makes that a
        //     one-line test instead of an evening.
        { std::lock_guard<std::mutex> dl(p->decoderMtx);
          j += std::string(",\"decoderKind\":\"")
             + (p->wefax ? "wefax" : p->sstv ? "sstv" : p->decoder ? "fsk" : "none") + "\""; }
        { std::lock_guard<std::mutex> dl(p->decoderMtx);
          if (p->decoder) {
              char db[160];
              snprintf(db, sizeof db,
                       ",\"decoder\":{\"resyncs\":%lu,\"audioLevel\":%.1f,"
                       "\"audioThreshold\":%.1f,\"state\":%d}",
                       p->decoder->resyncs(), p->decoder->audioLevel(),
                       p->decoder->audioThreshold(), p->decoder->stateNow());
              j += db;
          } }
    }
    { std::lock_guard<std::mutex> lk(g_vsMaintMtx);
      j += ",\"maintenance\":\"" + vibeadmin::esc(g_vsMaintActions) + "\""; }
    // ★ The schedule, so the page shows what is actually set rather than what it last sent.
    j += ",\"updateSrvHour\":" + std::to_string(g_vsUpdSrvHour.load())
       + ",\"updateSrvDay\":"  + std::to_string(g_vsUpdSrvDay.load())
       + ",\"updateAllHour\":" + std::to_string(g_vsUpdAllHour.load())
       + ",\"updateAllDay\":"  + std::to_string(g_vsUpdAllDay.load());
    j += std::string(",\"publicSharing\":") + (g_vsPublicSharing.load() ? "true" : "false");
    j += ",\"adminIdleMin\":" + std::to_string(g_vsAdminIdleMin.load());
    j += ",\"sessionLimitMin\":" + std::to_string(g_vsSessionLimitMin.load());
    // ★ Say plainly that there is no shell here, so a page never has to guess whether the
    //   button is missing because the server is old or because we chose not to ship one.
    j += ",\"terminal\":false";
    return j + "}";
}

/** The live listeners. ★★ One row per LISTENER, keyed on the spectrum socket, because that is
 *  what a slot actually is — the audio socket is optional and arrives second. */
std::string LocalSdrShim::adminSessionsJson() {
    if (!p) return "{\"sessions\":[]}";
    std::string j = "{\"sessions\":[";
    bool first = true;
    // ★★★ SNAPSHOT THE BYTE COUNTERS BEFORE TAKING clientMtx, NEVER UNDER IT. Reading them inside
    //     the loop meant holding clientMtx and then taking outboxMtx — a nesting order nothing
    //     else in this file uses, and this codebase has already lost an afternoon to a clientMtx
    //     deadlock that only appeared with more than one listener. Nothing here is worth a lock
    //     order that has to be reasoned about: one pass, then let go.
    // ★★★ EVERY LOOKUP THIS FUNCTION NEEDS IS DONE BEFORE clientMtx IS TAKEN, AND NOTHING IS
    //     CALLED WHILE HOLDING IT. Calling decoderOwner() from inside the loop SELF-DEADLOCKED —
    //     it takes clientMtx itself, and std::mutex is not recursive. It only fired with a
    //     per-client pipeline present, so the two single-user radios looked fine while the shared
    //     one wedged the moment somebody listened: every path touching clientMtx hung, and the
    //     process stayed "active" and kept accepting connections, which is the same disguise the
    //     LAST clientMtx deadlock wore (see BUG-vibeserver-broadcast-blocks / multiuser_was_three_bugs).
    // ★ Snapshotted here with the rest, and NOT under clientMtx — see the note below.
    std::string visitorsJson;
    {
        const double nowEpoch = (double)::time(nullptr);
        std::lock_guard<std::mutex> vl(g_vsVisitorMtx);
        bool vfirst = true;
        for (const auto& kv : g_vsVisitors) {
            if (nowEpoch - kv.second > 60.0) continue;
            if (!vfirst) visitorsJson += ',';
            vfirst = false;
            visitorsJson += "{\"ip\":\"" + vibeadmin::esc(kv.first) + "\""
                          + ",\"cc\":\"" + vibeadmin::esc(vsCountry(kv.first)) + "\""
                          + ",\"secs\":" + std::to_string((long long)(nowEpoch - kv.second)) + "}";
        }
    }
    std::string curDecoder;
    { std::lock_guard<std::mutex> dl(p->decoderMtx); curDecoder = p->currentDecoder; }
    std::map<net::Socket*, unsigned long long> sentBySock;
    {
        std::lock_guard<std::mutex> ol(p->outboxMtx);
        for (auto& kv : p->outboxes)
            if (kv.second) sentBySock[kv.first] = kv.second->sentTotal.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lk(p->clientMtx);
    const double now = Impl::nowSecs();
    for (auto& kv : p->clientDsp) {
        auto& c = kv.second;
        if (!c || !c->spec || !c->spec->isOpen()) continue;
        if (!first) j += ',';
        first = false;
        const bool isOccupant = !p->occupantSession.empty() && p->occupantSession == c->session;

        // ── What this listener costs, as a RATE ────────────────────────────────────────────
        const unsigned long long nowNanos = c->dspNanos.load(std::memory_order_relaxed);
        unsigned long long nowBytes = 0;
        for (auto* sk : { c->spec.get(), c->audio.get() }) {
            if (!sk) continue;
            auto it = sentBySock.find(sk);
            if (it != sentBySock.end()) nowBytes += it->second;
        }
        int cpuPct = -1, kbps = -1;
        if (c->lastSampleAt > 0) {
            const double dt = now - c->lastSampleAt;
            if (dt > 0.2) {
                cpuPct = (int)(((double)(nowNanos - c->lastDspNanos) / 1e9) / dt * 100.0 + 0.5);
                kbps   = (int)(((double)(nowBytes - c->lastSentBytes) * 8.0 / 1000.0) / dt + 0.5);
                c->lastDspNanos = nowNanos; c->lastSentBytes = nowBytes; c->lastSampleAt = now;
            }
        } else {
            c->lastDspNanos = nowNanos; c->lastSentBytes = nowBytes; c->lastSampleAt = now;
        }
        // ★ Only the decoder OWNER is actually decoding — the rest pay nothing for it, and saying
        //   otherwise would make every listener look like they had one running.
        //   ★★ Decided from state we ALREADY hold the lock for (decoderSession is guarded by
        //      clientMtx), not by calling decoderOwner() — see the note above.
        std::string dec;
        if (!curDecoder.empty()) {
            const bool owns = !p->decoderSession.empty()
                                ? (c->session == p->decoderSession)
                                : (p->specClient && p->specClient.get() == kv.first);
            if (owns) dec = curDecoder;
        }
        j += "{\"session\":\"" + vibeadmin::esc(c->session) + "\""
           + ",\"ip\":\"" + vibeadmin::esc(c->spec->peerAddress()) + "\""
           + ",\"vfoHz\":" + std::to_string((long long)c->vfoHz)
           + ",\"mode\":\"" + vibeadmin::esc(c->mode) + "\""
           + ",\"bwHz\":" + std::to_string((long long)c->bwHz)
           + ",\"audio\":" + ((c->audio && c->audio->isOpen()) ? "true" : "false")
           + ",\"opus\":" + (c->wantsOpus ? "true" : "false")
           // ★ How far behind this listener's own DSP thread has fallen. The one per-listener
           //   number that says WHOSE link is the problem when the server is struggling.
           + ",\"dropped\":" + std::to_string((unsigned long long)c->dropped.load())
           + ",\"zoomed\":" + (c->ownView ? "true" : "false")
           + ",\"cc\":\"" + vibeadmin::esc(vsCountry(c->spec->peerAddress())) + "\""
           + ",\"net\":\"" + vibeadmin::esc(vsAsnLabel(c->spec->peerAddress())) + "\""
           + ",\"agent\":\"" + vibeadmin::esc(c->agent.substr(0, 160)) + "\""
           // ★★ WHAT THIS ONE LISTENER COSTS. Both are rates derived from two samples of a
           //    monotonic counter — the first poll shows nothing, which is honest, rather than an
           //    average since connect that hides what is happening NOW.
           + ",\"cpu\":" + std::to_string(cpuPct)
           + ",\"kbps\":" + std::to_string(kbps)
           + ",\"decoder\":\"" + vibeadmin::esc(dec) + "\""
           + ",\"occupant\":" + (isOccupant ? "true" : "false")
           // ★★★ SAY WHICH SESSION IS AN ADMIN ONE. The live LISTENERS table had no admin field at
           //     all, so an owner sitting in their own admin session could not see it there — and
           //     neither could they see a STRANGER in one, which is the case that matters: an
           //     admin session is what a compromised password buys, and it is exempt from the very
           //     limits the table exists to police (Stuart, 2026-08-13). The connection HISTORY has
           //     badged admin since 08-12; the live table never did.
           // ★ Only ever on the OCCUPANT: `adminOk` is cleared whenever the spectrum client
           //   changes, so it describes the session in the chair and nobody else.
           + ",\"admin\":" + ((isOccupant && p->adminOk.load()) ? "true" : "false");
        if (isOccupant && p->occupantSince > 0)
            j += ",\"secs\":" + std::to_string((long long)(now - p->occupantSince));
        j += "}";
    }
    // ★★★ A SINGLE-USER RADIO HAS NO ClientDsp AT ALL. Per-client pipelines only exist when the
    //     centre is locked AND more than one listener is allowed, so on the Airspy and the dongle
    //     the loop above iterates an empty map and the radio reports "nobody listening" while
    //     somebody is plainly listening. Their occupant lives in the Impl's own state instead, so
    //     it is reported here — the owner is asking about PEOPLE, and should not have to know
    //     which DSP shape a radio happens to use.
    // ★ CPU is the whole process's DSP load, and on a single-user radio that is honest: all of it
    //   is being spent on this one listener.
    if (p->clientDsp.empty() && p->specClient && p->specClient->isOpen()) {
        unsigned long long bytes = 0;
        for (auto* sk : { p->specClient.get(), p->audioClient.get() }) {
            if (!sk) continue;
            auto it = sentBySock.find(sk);
            if (it != sentBySock.end()) bytes += it->second;
        }
        int kbps = -1;
        if (p->soleLastAt > 0) {
            const double dt = now - p->soleLastAt;
            if (dt > 0.2) {
                kbps = (int)(((double)(bytes - p->soleLastBytes) * 8.0 / 1000.0) / dt + 0.5);
                p->soleLastBytes = bytes; p->soleLastAt = now;
            }
        } else { p->soleLastBytes = bytes; p->soleLastAt = now; }

        const std::string ip = p->occupantAddr.empty() ? p->specClient->peerAddress() : p->occupantAddr;
        if (!first) j += ',';
        first = false;
        j += "{\"session\":\"" + vibeadmin::esc(p->occupantSession) + "\""
           + ",\"ip\":\"" + vibeadmin::esc(ip) + "\""
           + ",\"vfoHz\":" + std::to_string((long long)p->audioFreq.load())
           + ",\"mode\":\"" + vibeadmin::esc(p->mode) + "\""
           + ",\"audio\":" + ((p->audioClient && p->audioClient->isOpen()) ? "true" : "false")
           + ",\"dropped\":0,\"zoomed\":false"
           + ",\"cc\":\"" + vibeadmin::esc(vsCountry(ip)) + "\""
           + ",\"net\":\"" + vibeadmin::esc(vsAsnLabel(ip)) + "\""
           + ",\"agent\":\"" + vibeadmin::esc(p->occupantAgent.substr(0, 160)) + "\""
           + ",\"cpu\":" + std::to_string((int)(p->dspLoadPct + 0.5))
           + ",\"kbps\":" + std::to_string(kbps)
           + ",\"decoder\":\"" + vibeadmin::esc(curDecoder) + "\""
           + ",\"occupant\":true"
           // ★★★ AND BADGE IT HERE TOO. The per-client row above has said `admin` since 08-13; this
           //     one — the row used by every SINGLE-USER radio, which is the Airspy HF+ and the
           //     dongle — omitted the field entirely, and the admin page only badges on a truthy
           //     `admin`. So on those radios the badge could NEVER appear, for the owner or for a
           //     stranger holding a compromised password. A protection that is absent on two of the
           //     three radios is the same shape as a control that only works on one of them.
           + ",\"admin\":" + (p->adminOk.load() ? "true" : "false");
        if (p->occupantSince > 0)
            j += ",\"secs\":" + std::to_string((long long)(now - p->occupantSince));
        j += "}";
    }

    // ★★ THE PEOPLE WHO ARE NOT ON YET. A queue is invisible in a listener list by definition, and
    //    "nobody is listening" reads very differently from "nobody is listening and four are
    //    waiting". Position is 1-based and is the order that will actually be honoured.
    j += "],\"queue\":[";
    for (size_t i = 0; i < p->waitQueue.size(); ++i) {
        const auto& w = p->waitQueue[i];
        if (i) j += ',';
        j += "{\"pos\":" + std::to_string(i + 1)
           + ",\"ip\":\"" + vibeadmin::esc(w.ip) + "\""
           + ",\"cc\":\"" + vibeadmin::esc(vsCountry(w.ip)) + "\""
           + ",\"secs\":" + std::to_string((long long)(now - w.since)) + "}";
    }
    j += "],\"visitors\":[" + visitorsJson + "],\"adminOk\":" + std::string(p->adminOk.load() ? "true" : "false");
    return j + "}";
}

/** Close a listener's sockets. Empty `session` matches on address instead, which is what the
 *  ban path needs. Returns how many were closed. */
int LocalSdrShim::adminKick(const std::string& session, const std::string& ip) {
    if (!p || (session.empty() && ip.empty())) return 0;
    std::vector<std::shared_ptr<net::Socket>> doomed;
    std::vector<std::pair<std::string, std::string>> logged;   // ip, session
    {
        std::lock_guard<std::mutex> lk(p->clientMtx);
        for (auto& kv : p->clientDsp) {
            auto& c = kv.second;
            if (!c || !c->spec) continue;
            const std::string addr = c->spec->peerAddress();
            const bool hit = (!session.empty() && c->session == session)
                          || (session.empty() && !ip.empty() && addr == ip);
            if (!hit) continue;
            doomed.push_back(c->spec);
            if (c->audio) doomed.push_back(c->audio);
            logged.emplace_back(addr, c->session);
            if (p->occupantSession == c->session) {
                p->occupantSession.clear(); p->occupantSince = 0; p->occupantAddr.clear();
            }
        }
    }
    // ★★ TELL THEM, THEN CLOSE — and OUTSIDE clientMtx. A blocking send under the lock the DSP
    //    thread needs is the shape of the freeze that took a whole session to find (see the
    //    per-client outbox note). A client that is merely dropped auto-reconnects; one that is
    //    told `kicked` treats it as terminal and stops.
    static const char* kMsg = "{\"type\":\"kicked\"}";
    for (auto& s : doomed) {
        if (!s || !s->isOpen()) continue;
        p->sendWs(s, 0x1, (const uint8_t*)kMsg, strlen(kMsg));
        s->close();
    }
    for (auto& e : logged) g_vsConnLog.close(e.first, e.second, "kicked");
    return (int)logged.size();
}

/** Kick everyone the given ban rule now matches — the other half of "a ban must take effect on
 *  people who are already here". */
void LocalSdrShim::broadcastNotice() { if (p) p->sendNoticeNow(); }

int LocalSdrShim::adminKickMatching(const std::string& cidr) {
    if (!p) return 0;
    vibeadmin::Ban rule;
    rule.cidr = cidr;
    if (!vibeadmin::compile(rule)) return 0;
    std::vector<std::string> hits;
    {
        std::lock_guard<std::mutex> lk(p->clientMtx);
        for (auto& kv : p->clientDsp) {
            auto& c = kv.second;
            if (!c || !c->spec) continue;
            const std::string addr = c->spec->peerAddress();
            // ★★ AN ASN RULE MATCHES BY NETWORK, NOT BY ADDRESS — vibeadmin::matches() returns
            //    false for one by design (its `net` is meaningless). Without this branch a
            //    "block this network" would store the rule, report success, and leave everyone
            //    it names still listening: the precise failure the ban-then-kick step exists to
            //    prevent, reintroduced one rule type later.
            bool hit = false;
            if (rule.asn) {
                LocalSdrShim::AsnFn fn;
                { std::lock_guard<std::mutex> cl(g_vsConfigMtx); fn = g_vsAsnFn; }
                uint32_t a = 0; std::string nm;
                hit = fn && fn(addr, a, nm) && a == rule.asn;
            } else {
                hit = vibeadmin::matches(rule, addr);
            }
            if (hit) hits.push_back(c->session);
        }
    }
    int n = 0;
    for (auto& s : hits) n += adminKick(s, "");
    return n;
}

/** ★★★ THE FOUR BUTTONS THAT REPLACE A TERMINAL. See the routing comment for why this is a
 *  fixed list and not an exec endpoint.
 *  ★★ The actions are performed by the DAEMON, not here: this file is compiled into the phone
 *  app too, and an Android build has no systemd, no apt and no business rebooting anything. A
 *  build with no handler registered answers "not supported on this server", which is the
 *  truth rather than a silent no-op. */
bool LocalSdrShim::adminAction(const std::string& action, std::string& err) {
    AdminActionFn fn;
    { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsAdminActionFn; }
    if (!fn) { err = "this server cannot perform maintenance actions"; return false; }
    // ★★ ENFORCED, not merely undrawn. The advertised list is what the page uses to decide what
    //    to show; this is what makes it true. A client that asks anyway — an old build, a script,
    //    a curious person — must be refused, not obeyed.
    { std::lock_guard<std::mutex> lk(g_vsMaintMtx);
      if (g_vsMaintActions.find(action) == std::string::npos) {
          err = "this server does not offer that action";
          return false;
      } }
    if (action != "reboot" && action != "restart" && action != "update-check" &&
        action != "update" && action != "update-all" && action != "shutdown") {
        err = "unknown action: " + action;
        return false;
    }
    return fn(action, err);
}
void LocalSdrShim::setAdminActionHandler(AdminActionFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsAdminActionFn = std::move(fn);
}
void LocalSdrShim::setAdminLogHandler(AdminLogFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsAdminLogFn = std::move(fn);
}
void LocalSdrShim::setGeoIpHandler(GeoIpFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsGeoIpFn = std::move(fn);
}
void LocalSdrShim::setAsnHandler(AsnFn fn) {
    { std::lock_guard<std::mutex> lk(g_vsConfigMtx); g_vsAsnFn = std::move(fn); }
    // The ban list resolves ASN rules through this. Wired here so there is ONE place the handler
    // is installed and no way to register the lookup but forget the enforcement.
    g_vsBans.setAsnResolver([](const std::string& ip, uint32_t& asn) {
        LocalSdrShim::AsnFn f;
        { std::lock_guard<std::mutex> lk(g_vsConfigMtx); f = g_vsAsnFn; }
        std::string name;
        return f ? f(ip, asn, name) : false;
    });
}
/** Network for an address: "AS15169 GOOGLE", or empty when unknown. */
static std::string vsAsnLabel(const std::string& ip) {
    LocalSdrShim::AsnFn fn;
    { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsAsnFn; }
    if (!fn || ip.empty()) return {};
    uint32_t asn = 0; std::string name;
    if (!fn(ip, asn, name) || !asn) return {};
    return "AS" + std::to_string(asn) + (name.empty() ? "" : " " + name);
}
/** Country for an address, or empty. Never throws and never blocks on the network. */
static std::string vsCountry(const std::string& ip) {
    LocalSdrShim::GeoIpFn fn;
    { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsGeoIpFn; }
    if (!fn || ip.empty()) return {};
    return fn(ip);
}

int LocalSdrShim::occupantSecsLeft(int adminOverride) const {
    const int limitMin = g_vsSessionLimitMin.load();
    if (!p || limitMin <= 0) return -1;
    // ★★★ WHOSE ADMIN? The exemption is the ASKING listener's, not the radio's. With one flag for
    //     the whole process this could only ever answer for everybody, so on a shared receiver an
    //     admin's countdown depended on whether some other listener happened to be unlocked.
    //     -1 = "no answer supplied, use the radio-wide flag", which is right for the callers that
    //     have no particular listener in mind (the /vibeserver.json probe).
    const bool exempt = adminOverride >= 0 ? adminOverride != 0 : p->adminOk.load();
    if (exempt) return -1;                               // owner: exempt, so no countdown
    std::lock_guard<std::mutex> lk(p->clientMtx);
    if (p->occupantSession.empty() || p->occupantSince <= 0) return -1;
    if (p->occupantAddr.empty() || isLoopback(p->occupantAddr)) return -1;
    const double left = (double)limitMin * 60.0 - (Impl::nowSecs() - p->occupantSince);
    return left > 0 ? (int)(left + 0.5) : 0;
}

void LocalSdrShim::setVibeServerWebEnabled(bool on) { g_vsWebEnabled.store(on); }
void LocalSdrShim::setVibeServerLockedRate(double rate) { g_vsLockedRate.store(rate > 0 ? rate : 0.0); }
void LocalSdrShim::setVibeServerLockedCentre(double hz) { g_vsLockedCentre.store(hz > 0 ? hz : 0.0); }
void LocalSdrShim::setVibeServerSharedChannels(bool shared) { g_vsSharedChannels.store(shared); }
void LocalSdrShim::setVibeServerZoomSpectrum(bool on) { g_vsZoomSpectrum.store(on); }
void LocalSdrShim::setVibeServerIdleGrace(double sec) { g_vsIdleGraceSec.store(sec < 0 ? 0 : sec); }
void LocalSdrShim::setVibeServerReleaseWhenIdle(bool on) { g_vsReleaseWhenIdle.store(on); }
void LocalSdrShim::setVibeServerRfNotch(bool on)  { g_vsRfNotch.store(on); }
void LocalSdrShim::setProvidesSpectrogram(bool on) { g_vsProvidesSpectrogram.store(on); }

void LocalSdrShim::setBandRegion(int region) {
    vibebands::defaultRegion() = (region >= 1 && region <= 3) ? region : 1;
    LOGI("band plan: ITU region %d", vibebands::defaultRegion());
}

void LocalSdrShim::setVibeServerTuneLimits(const std::string& allowCsv, const std::string& blockCsv) {
    std::lock_guard<std::mutex> lk(g_vsTuneLimitMtx);
    g_vsAllowCsv = allowCsv;
    g_vsBlockCsv = blockCsv;
    g_vsPermitted.clear();          // recomputed against the hardware's coverage on first use
    if (!allowCsv.empty() || !blockCsv.empty())
        LOGI("tuning limits: allow[%s] block[%s]",
             allowCsv.empty() ? "everything" : allowCsv.c_str(),
             blockCsv.empty() ? "nothing" : blockCsv.c_str());
}

/** The permitted set for THIS radio, hardware coverage included. Recomputed when the lists change
 *  or the coverage is first known — the driver may not have reported it when the lists arrived. */
static vibebands::Ranges vsPermittedRanges(const vibebands::Ranges& hardware) {
    std::lock_guard<std::mutex> lk(g_vsTuneLimitMtx);
    if (g_vsAllowCsv.empty() && g_vsBlockCsv.empty()) return {};
    if (g_vsPermitted.empty())
        g_vsPermitted = vibebands::permitted(hardware, g_vsAllowCsv, g_vsBlockCsv);
    return g_vsPermitted;
}
void LocalSdrShim::setVibeServerDabNotch(bool on) { g_vsDabNotch.store(on); }
void LocalSdrShim::setVibeServerMaxUsers(int n) { g_vsMaxUsers.store(n > 1 ? n : 1); }

void LocalSdrShim::setConfigHandlers(ConfigGetFn get, ConfigSetFn set) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsConfigGet = std::move(get);
    g_vsConfigSet = std::move(set);
}
/** ★★★ A SERVER WITH NO RADIO — THE FRONT DOOR.
 *
 *  It holds the one port that leaves the machine, lists the radios, serves the setup and admin
 *  pages, and hands every listener's connection to the radio process they asked for. It owns no
 *  device at all.
 *
 *  ★★ AND THAT IS THE POINT, not a side effect. Stuart, 2026-08-08: "the 48000 page can still
 *     serve if for some reason all the radios fail, an admin can still gain entry and reboot the
 *     system". A front door that dies with the radios is exactly no use on the day you need it —
 *     an appliance in a cupboard whose every receiver has failed is precisely when someone needs
 *     to get in and restart it.
 *
 *  ★ Whitelisted, deliberately. Almost every handler below assumes a device: they read gain, span,
 *    the FFT, the front end. Serving them here would mean auditing each one for a null radio and
 *    getting it right for ever. Answering 503 for anything that needs a radio is smaller, safer,
 *    and honest — this process genuinely cannot answer those.
 */
int LocalSdrShim::startFrontDoor(int port, std::string& err) {
    std::lock_guard<std::mutex> life(g_lifecycle);
    if (instance().p) { err = "already running"; return -1; }
    Impl* impl = new Impl();
    impl->frontDoorOnly = true;

    int chosen = -1;
    if (port > 0) {
        try { impl->listener = net::listen(bindHost(), port); chosen = port; }
        catch (...) { impl->listener = nullptr; }
    } else {
        for (int p2 = 48000; p2 < 48050; p2++) {
            try { impl->listener = net::listen(bindHost(), p2); chosen = p2; break; }
            catch (...) { impl->listener = nullptr; }
        }
    }
    if (!impl->listener) {
        err = port > 0 ? "port " + std::to_string(port) + " is already in use — choose another"
                       : "no free port in 48000-48049";
        delete impl; return -1;
    }
    impl->port = chosen;
    impl->serverRunning.store(true);
    impl->acceptThread = std::thread([impl]{ impl->acceptLoop(); });
    instance().p = impl;
    return chosen;
}

void LocalSdrShim::setHandoffRouter(HandoffFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsHandoffFn = std::move(fn);
}

void LocalSdrShim::setPathPrefix(const std::string& prefix, const std::string& alt) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsPathPrefix = prefix;
    g_vsPathPrefixAlt = alt;
}

bool LocalSdrShim::listenForHandoff(const std::string& socketPath, std::string& err) {
    if (!instance().p) { err = "server not running"; return false; }
    Impl* impl = instance().p;
    impl->handoffFd = vibe::fdListen(socketPath, err);
    if (impl->handoffFd < 0) return false;
    impl->handoffPath = socketPath;
    impl->handoffThread = std::thread([impl]{ impl->handoffLoop(); });
    LOGI("hand-off socket: %s", socketPath.c_str());
    return true;
}

void LocalSdrShim::setRadiosHandler(RadiosFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsRadiosFn = std::move(fn);
}

void LocalSdrShim::setSolarHandler(SolarFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsSolarFn = std::move(fn);
}
void LocalSdrShim::setLogoCacheClearHandler(LogoCacheClearFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsLogoClearFn = std::move(fn);
}
void LocalSdrShim::clearLogoCache() {
    LogoCacheClearFn fn;
    { std::lock_guard<std::mutex> lk(g_vsConfigMtx); fn = g_vsLogoClearFn; }
    if (fn) fn();
}
void LocalSdrShim::setStationLogoHandler(StationLogoFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsStationLogoFn = std::move(fn);
}
void LocalSdrShim::setEibiHandler(EibiFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsEibiFn = std::move(fn);
}
void LocalSdrShim::setRtlSerialHandler(RtlSerialFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsRtlSerialFn = std::move(fn);
}

void LocalSdrShim::setRtlSerialStatusHandler(RtlSerialStatusFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsRtlSerialStatusFn = std::move(fn);
}

void LocalSdrShim::setTrustedProxies(const std::string& csv) {
    g_vsHaveTrustedProxies.store(!csv.empty());
    std::vector<std::string> entries;
    size_t start = 0;
    while (start <= csv.size()) {
        const size_t comma = csv.find(',', start);
        const size_t end = comma == std::string::npos ? csv.size() : comma;
        std::string e = csv.substr(start, end - start);
        const size_t a = e.find_first_not_of(" \t");
        const size_t b = e.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) entries.push_back(e.substr(a, b - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    {
        std::lock_guard<std::mutex> lk(g_vsTrustedProxiesMtx);
        g_vsTrustedProxies.set(entries);
    }
    // ★ Say it out loud at startup. A wrong entry here is invisible until somebody is banned who
    //   should not have been, so the owner should be able to see what the server believes.
    if (!entries.empty())
        LOGI("trusting X-Forwarded-For from %d proxy entr%s", (int)entries.size(),
             entries.size() == 1 ? "y" : "ies");
}

void LocalSdrShim::setConfigPersistHandler(ConfigPersistFn fn) {
    std::lock_guard<std::mutex> lk(g_vsConfigMtx);
    g_vsConfigPersist = std::move(fn);
}
void LocalSdrShim::setVibeServerSavedFrontEnd(int lnaState, int ifGr, int ifAgc) {
    g_vsSavedLna.store(lnaState); g_vsSavedIfGr.store(ifGr); g_vsSavedIfAgc.store(ifAgc);
}
void LocalSdrShim::setConfigured(bool on) { g_vsConfigured.store(on); }
void LocalSdrShim::setNativeSetup(bool on) { g_vsNativeSetup.store(on); }
void LocalSdrShim::setVibeServerLanding(double hz, const std::string& mode) {
    g_vsLandingHz.store(hz > 0 ? hz : 0.0);
    std::lock_guard<std::mutex> lk(g_vsLandingMtx);
    g_vsLandingMode = mode;
}
bool LocalSdrShim::isConfigured() { return g_vsConfigured.load(); }
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
    impl->baseFftRate = fftRate;   // the owner's default — the floor every listener rate is measured against
    // VibeServer waterfall frame-rate throttle: a serving host can cap fps (Full
    // 20 / Half 10 / Quarter 5) to save CPU and wire data. The client interpolates
    // the waterfall, so a lower rate still scrolls smoothly.
    if (g_serveOnLan.load()) {
        double mr = g_vsMaxFftRate.load();
        // ★ Cap the DEFAULT too, not just the running rate — it is what an unasking listener
        //   gets and what recomputeEngineRate() falls back to, so a ceiling that missed it would
        //   be re-breached the moment the last listener left.
        if (mr > 0 && mr < impl->fftRate) { impl->fftRate = mr; impl->baseFftRate = mr; }
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
    // ★★★ NEVER THE TUNER'S OWN AUTOMATIC GAIN. `gain < 0` used to mean "hardware AGC", and on an
    //     RTL that is a mode to stay out of: it is unreliable across tuners and is KNOWN BROKEN on
    //     the v4 (Stuart, 2026-08-06). An unset gain now means the LOWEST the tuner offers —
    //     manual, and the safe end — so a receiver whose owner has not chosen yet is quiet rather
    //     than at the mercy of a gain loop we do not control and cannot fix.
    //     ★ Quiet is recoverable in one click; an overloaded front end on an unknown aerial is
    //       not, and neither is a listener concluding the receiver is deaf because its AGC misbehaved.
    int applyGain = gainTenthDb;
    if (applyGain < 0) {
        int n = rtlsdr_get_tuner_gains(impl->dev, nullptr);
        if (n > 0) {
            std::vector<int> gs((size_t)n);
            rtlsdr_get_tuner_gains(impl->dev, gs.data());
            applyGain = *std::min_element(gs.begin(), gs.end());
            LOGI("gain: no setting yet — starting at the tuner's minimum, %.1f dB (never auto)",
                 applyGain / 10.0);
        }
    }
    impl->lastGainTenthDb = applyGain;   // re-applied if the dongle is replugged
    rtlsdr_set_tuner_gain_mode(impl->dev, 1);
    if (applyGain >= 0) rtlsdr_set_tuner_gain(impl->dev, applyGain);
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
    impl->baseFftRate = fftRate;   // the owner's default — the floor every listener rate is measured against
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
    impl->ahfIndex = index;   // ★ remembered so releaseRadio can reopen the same one
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
    impl->baseFftRate = fftRate;   // the owner's default — the floor every listener rate is measured against
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
    // ★★★ THE OPERATOR'S FRONT-END FILTERS, APPLIED BY THE SERVER ITSELF.
    //     These used to arrive only because the first client pushed its saved preferences at
    //     us — which stopped happening the moment listeners on a LOCKED receiver were correctly
    //     barred from setting shared gain. A receiver's own filter settings must not depend on
    //     a listener turning up and volunteering them (2026-08-03).
    //     ★★ AND THEY ARE OPT-IN, NOT A DEFAULT-ON. The RF notch covers BROADCAST FM, so
    //        forcing it on every locked receiver would gut an FM-DXer's signal — Hans runs a
    //        public one, on the FM band, and would have had it silently notched out. What is
    //        right for a 2.5-10.5 MHz HF demo is catastrophic one band up. The operator says.
    if (g_vsRfNotch.load())  { impl->sdrp->setRfNotch(true);  LOGI("RSP: RF notch ON (operator)"); }
    if (g_vsDabNotch.load()) { impl->sdrp->setDabNotch(true); LOGI("RSP: DAB notch ON (operator)"); }

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
    impl->baseFftRate = fftRate;   // the owner's default — the floor every listener rate is measured against
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
    impl->baseFftRate = fftRate;   // the owner's default — the floor every listener rate is measured against
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
    const uint32_t iqHz  = (uint32_t)llround(centerFreq + impl->hwOffsetHz());
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
      if (impl->dxClient) impl->dxClient->close();
      // ★ Per-client sockets too, for the same reason — a shared receiver has N of them and
      //   closing only the primary leaves the rest blocking their writers.
      for (auto& kv : impl->clientDsp) {
          if (kv.second->spec)  kv.second->spec->close();
          if (kv.second->audio) kv.second->audio->close();
      } }

    // ★★★ JOIN EVERY PER-CLIENT WRITER BEFORE THE MAPS GO. Each listener owns an outbox thread,
    //     and destroying a std::thread that is still joinable calls std::terminate — which is
    //     precisely what "terminate called without an active exception" and status=6/ABRT in the
    //     journal were: every single restart aborted on the way out. Harmless in itself, and
    //     exactly the kind of noise that hides a REAL crash the first time one happens.
    {
        std::vector<std::shared_ptr<net::Socket>> socks;
        { std::lock_guard<std::mutex> lk(impl->outboxMtx);
          for (auto& kv : impl->outboxes) socks.push_back(kv.second->sock); }
        for (auto& sk : socks) impl->outboxClose(sk, 0);      // no drain: we are going down
    }
    {
        std::vector<std::shared_ptr<Impl::ClientDsp>> gone;
        { std::lock_guard<std::mutex> lk(impl->clientMtx);
          for (auto& kv : impl->clientDsp) gone.push_back(kv.second);
          impl->clientDsp.clear(); impl->pendingAudio.clear(); impl->sockSession.clear(); }
        for (auto& c : gone) impl->stopClientThread(c);   // outside the lock — they take it too
        // ★ A departing listener may have been the RDS decoder for its frequency; hand it to
        //   whoever is still there. Cheap, and only when somebody actually left.
        if (!gone.empty()) impl->rdsResweep();
    }

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
    // ★★★ AND THE HAND-OFF LISTENER. Destroying a joinable std::thread calls std::terminate — which
    //     is exactly the "terminate called without an active exception" and status=6/ABRT that
    //     appeared on every radio's shutdown the moment this thread was added. It aborts on the way
    //     OUT, so the process looks fine until systemd restarts it and finds it failed.
    // ★ Closing the fd first is what makes the join prompt: the loop is sitting in poll(), and this
    //   wakes it rather than waiting out its timeout.
    if (impl->handoffFd >= 0) { ::close(impl->handoffFd); impl->handoffFd = -1; }
    joinSafely(impl->handoffThread, "hand-off thread");
    // ★ Leave no stale socket behind: the next process to start would bind() onto a file nothing
    //   is listening on, and every hand-off to it would fail with "connection refused".
    if (!impl->handoffPath.empty()) ::unlink(impl->handoffPath.c_str());
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
// ★ DEFINED HERE, above its FIRST user — it used to sit halfway down the file, next to the RSP
//   setters that were its only callers. The RTL setters above needed it too.
#define VIBE_HW_LOCK() std::lock_guard<std::recursive_mutex> _hwlk(p->modeMtx)

void LocalSdrShim::setGain(int gainTenthDb) {
    if (!p) return;
    // ★★ THE SAME LOCK THE RSP AND AIRSPY SETTERS ALREADY USE (VIBE_HW_LOCK). The RTL ones
    //    never took it, which is why reopenDevice() could never be called: a close could
    //    land mid-tune, and libusb turns that into an ABORT, not an error.
    // ★ A SECOND lock would only add an inversion to invert — modeMtx is already recursive
    //   and already the one held across engine rebuilds.
    VIBE_HW_LOCK();
    if (p->radioReleased.load()) return;   // the radio is lent to another program
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
    // ★★★ RECORD IT ON THIS PATH TOO. Every other radio's branch stores lastGainTenthDb and the
    //     dongle's did not, so the value the shim believed was current was whatever some earlier
    //     path had written. It is what gets re-applied after a stream restart (see the replug
    //     handler), and it is now what the client is TOLD the gain is — a shim that cannot say
    //     where its own gain is cannot correct a client that has guessed.
    p->lastGainTenthDb = gainTenthDb;
    if (gainTenthDb < 0) { rtlsdr_set_tuner_gain_mode(p->dev, 0); LOGI("gain: auto"); }
    else { rtlsdr_set_tuner_gain_mode(p->dev, 1); rtlsdr_set_tuner_gain(p->dev, gainTenthDb);
           LOGI("gain: %.1f dB", gainTenthDb / 10.0); }
}
/** The gain the radio is ACTUALLY set to, in its own units; -1 = auto/AGC. */
int LocalSdrShim::currentGainTenthDb() const { return p ? p->lastGainTenthDb : -1; }
/** ★★★ LET THE RADIO GO WITHOUT STOPPING THE SERVER.
 *
 *  For a machine where VibeServer shares one SDR with something else (OpenWebRX, a decoder), the
 *  existing idle park is no use: it stops CONSUMING samples but never drops the USB claim, and on
 *  the dongle it deliberately keeps the stream running and throws the buffers away, because
 *  cancelling and restarting it was what crashed the server. Nothing else can open the device.
 *
 *  ★★ ORDER IS COPIED FROM setSampleRate, WHICH IS PROVEN ON ALL THREE RADIOS: quiesce the
 *     source, join the capture thread, stop the DSP, and only THEN take modeMtx. Taking it any
 *     earlier deadlocks — stopDspThread() joins a thread that locks modeMtx per buffer.
 *  ★ The HTTP server, admin page, config and mDNS all stay up. What stops is anything that needs
 *    IQ: the spectrogram and the band-conditions measurement. That is the honest cost and the
 *    setup page says so.
 */
bool LocalSdrShim::releaseRadio() {
    if (!p) return false;
    Impl* impl = p;
    if (impl->radioReleased.load()) return true;
    // ★ A network source owns nothing local, so there is nothing to hand over.
    if (impl->useTcp() || impl->useSpy()) return false;
    const bool rsp = impl->useSdrplay(), ahf = impl->useAirspyHf();

    if (rsp)      impl->sdrp->setPaused(true);
    else if (ahf) impl->ahf->setPaused(true);
    else if (impl->dev) { impl->restarting.store(true); rtlsdr_cancel_async(impl->dev); }
    if (impl->rtlThread.joinable()) impl->rtlThread.join();
    impl->stopDspThread();
    {
        std::lock_guard<std::recursive_mutex> lk(impl->modeMtx);
        // ★★ radioReleased goes up INSIDE the lock and BEFORE the close, so a control call that
        //    is already waiting on modeMtx returns instead of touching a freed handle.
        impl->radioReleased.store(true);
        if (rsp)      impl->sdrp->close();
        else if (ahf) { impl->ahf->stop(); impl->ahf->close(); }
        else if (impl->dev) { rtlsdr_close(impl->dev); impl->dev = nullptr; }
        impl->restarting.store(false);
    }
    { std::lock_guard<std::mutex> lk(impl->iqMtx);
      impl->iqQueue.clear(); impl->iqQueuedSamples = 0; impl->iqPrefilled = false; }
    LOGI("radio RELEASED — another program may open it now (spectrogram and band conditions stop)");
    impl->notifyDeviceState();
    return true;
}

/** Take the radio back. Returns false when something else now holds it — which is a NORMAL
 *  outcome here, not an error: that is the whole point of having lent it out. The caller turns
 *  this into a message a listener can act on rather than an empty waterfall. */
bool LocalSdrShim::radioIsReleased() const { return p && p->radioReleased.load(); }

bool LocalSdrShim::reacquireRadio(std::string& err) {
    if (!p) { err = "server not running"; return false; }
    Impl* impl = p;
    if (!impl->radioReleased.load()) return true;
    const bool rsp = impl->useSdrplay(), ahf = impl->useAirspyHf();
    std::lock_guard<std::recursive_mutex> lk(impl->modeMtx);
    // ★★★ ASK AGAIN, NOW THAT WE HOLD THE LOCK. The check above is outside it, and a browser opens
    //     its spectrum and audio sockets together: both arrive, both see the radio released, the
    //     first takes the lock and reacquires — and the second, already past that check, waits for
    //     the mutex and then opens a device THIS PROCESS ALREADY HOLDS. Twelve
    //     usb_claim_interface -6 in a row (the retry loop below doing its best), then "the radio is
    //     in use by another program on this machine", which was true only in the sense that we were
    //     the other program. Saber's log shows REACQUIRED immediately followed by the storm
    //     (2026-08-09) — that is the whole bug, and it is why his receiver gave a second of
    //     spectrum and then died.
    // ★★ Textbook double-checked locking: the fast path outside the lock is only safe if the slow
    //    path repeats the test inside it. Cheap, and it makes concurrent arrivals a no-op rather
    //    than a fight.
    if (!impl->radioReleased.load()) return true;

    // ★★★ "BUSY" IS OFTEN JUST "NOT YET". Taking the radio back a moment after letting it go —
    //     which is the normal shape of this feature — can land while the kernel still has our own
    //     interface claimed, or while the other program is halfway through its own close. The open
    //     then fails with LIBUSB_ERROR_BUSY (-6) and we reported it as "the radio is in use by
    //     another program on this machine", which was simply untrue and left the listener with a
    //     dead receiver and nothing retrying.
    //
    // ★★★ SABER'S LOG, 2026-08-09, reads exactly this way: `radio REACQUIRED`, a session landing,
    //     and then `usb claim interface error -6` followed by `could not take the radio back`.
    //     It "worked for a second and then froze" — because the retake after the next release had
    //     one attempt and gave up. His kernel is a Termux one that even logs a usbfs mmap
    //     work-around, so the window is wider there than on a Pi.
    //
    // ★ Three seconds of patience, then the honest message. Long enough to cover a hand-over,
    //   short enough that a radio genuinely held by OpenWebRX still says so promptly.
    const int kTries = 12;
    const int kWaitMs = 250;
    bool ok = false;
    for (int attempt = 0; attempt < kTries && !ok; attempt++) {
      if (attempt) {
          std::this_thread::sleep_for(std::chrono::milliseconds(kWaitMs));
          err.clear();
      }
    if (rsp) {
        ok = impl->sdrp->open(impl->sdrpIndex, impl->sampleRate, impl->rtlCenter.load(),
                              impl->lastGainTenthDb, err);
    } else if (ahf) {
        ok = impl->ahf->open(impl->ahfIndex, impl->sampleRate, impl->rtlCenter.load(),
                             impl->lastGainTenthDb, err);
    } else {
        // ★ BY SERIAL, NOT BY INDEX — findOurDevice refuses to grab a DIFFERENT dongle that has
        //   taken our slot while we were away. With three radios on one machine that matters.
        const int idx = impl->findOurDevice();
        if (idx < 0) { err = "the radio is not there"; }
        else if (rtlsdr_open(&impl->dev, (uint32_t)idx) != 0 || !impl->dev) {
            impl->dev = nullptr;
            err = "the radio is in use by another program on this machine";
        } else {
            rtlsdr_set_sample_rate(impl->dev, (uint32_t)impl->sampleRate);
            impl->tuneHw(impl->rtlCenter.load());
            if (impl->lastGainTenthDb < 0) rtlsdr_set_tuner_gain_mode(impl->dev, 0);
            else { rtlsdr_set_tuner_gain_mode(impl->dev, 1);
                   rtlsdr_set_tuner_gain(impl->dev, impl->lastGainTenthDb); }
            rtlsdr_reset_buffer(impl->dev);
            ok = true;
        }
    }
    }   // ★ end of the retry loop
    if (!ok) {
        if (err.empty()) err = "the radio is in use by another program on this machine";
        LOGI("could not take the radio back — %s", err.c_str());
        return false;
    }

    impl->radioReleased.store(false);
    impl->startEngine();
    impl->buildAudio();
    impl->startDspThread();
    if (rsp)      impl->sdrp->setPaused(false);
    else if (ahf) { std::string e2; impl->ahf->start(e2); impl->ahf->setPaused(false); }
    else          impl->launchCapture();
    LOGI("radio REACQUIRED");
    impl->notifyDeviceState();
    return true;
}

void LocalSdrShim::setPpm(int ppm) {
    if (!p) return;
    // ★★ THE SAME LOCK THE RSP AND AIRSPY SETTERS ALREADY USE (VIBE_HW_LOCK). The RTL ones
    //    never took it, which is why reopenDevice() could never be called: a close could
    //    land mid-tune, and libusb turns that into an ABORT, not an error.
    // ★ A SECOND lock would only add an inversion to invert — modeMtx is already recursive
    //   and already the one held across engine rebuilds.
    VIBE_HW_LOCK();
    if (p->radioReleased.load()) return;   // the radio is lent to another program
    if (p->useSpy()) return;   // no ppm setting in the SpyServer protocol

    if (p->useTcp()) { p->sendTcpCmd(0x05, (uint32_t)ppm); return; }
    if (!p->dev) return;
    rtlsdr_set_freq_correction(p->dev, ppm); LOGI("ppm: %d", ppm);
}
void LocalSdrShim::setBiasTee(bool on) {
    if (!p) return;
    // ★★ THE SAME LOCK THE RSP AND AIRSPY SETTERS ALREADY USE (VIBE_HW_LOCK). The RTL ones
    //    never took it, which is why reopenDevice() could never be called: a close could
    //    land mid-tune, and libusb turns that into an ABORT, not an error.
    // ★ A SECOND lock would only add an inversion to invert — modeMtx is already recursive
    //   and already the one held across engine rebuilds.
    VIBE_HW_LOCK();
    if (p->radioReleased.load()) return;   // the radio is lent to another program
    if (p->useTcp()) { p->sendTcpCmd(0x0e, on ? 1 : 0); return; }
    if (!p->dev) return;
    rtlsdr_set_bias_tee(p->dev, on ? 1 : 0);
    g_biasTeeOn.store(on);
    LOGI("bias-tee: %s", on ? "ON — DC on the feedline" : "off");
}
void LocalSdrShim::setAgc(bool on) {
    if (!p) return;
    // ★★ THE SAME LOCK THE RSP AND AIRSPY SETTERS ALREADY USE (VIBE_HW_LOCK). The RTL ones
    //    never took it, which is why reopenDevice() could never be called: a close could
    //    land mid-tune, and libusb turns that into an ABORT, not an error.
    // ★ A SECOND lock would only add an inversion to invert — modeMtx is already recursive
    //   and already the one held across engine rebuilds.
    VIBE_HW_LOCK();
    if (p->radioReleased.load()) return;   // the radio is lent to another program
    if (p->useTcp()) { p->sendTcpCmd(0x08, on ? 1 : 0); return; }
    if (!p->dev) return;
    rtlsdr_set_agc_mode(p->dev, on ? 1 : 0); LOGI("agc: %d", on);
}
void LocalSdrShim::setDirectSampling(int mode) {
    if (!p) return;
    // ★★ THE SAME LOCK THE RSP AND AIRSPY SETTERS ALREADY USE (VIBE_HW_LOCK). The RTL ones
    //    never took it, which is why reopenDevice() could never be called: a close could
    //    land mid-tune, and libusb turns that into an ABORT, not an error.
    // ★ A SECOND lock would only add an inversion to invert — modeMtx is already recursive
    //   and already the one held across engine rebuilds.
    VIBE_HW_LOCK();
    if (p->radioReleased.load()) return;   // the radio is lent to another program
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
void LocalSdrShim::setWeakProc(bool on) {
    g_dsp.weakProcOn.store(on);
    if (!p) return;
    p->weakProcOn.store(on);
    // ★ Applied to EVERY pipeline this process owns — the Impl's own and each per-client one — or
    //   the switch would work on a single-user radio and silently do nothing on a shared one.
    p->rx.setWeakSignalProc(on);
    { std::lock_guard<std::mutex> lk(p->clientMtx);
      for (auto& kv : p->clientDsp) if (kv.second && kv.second->rx) kv.second->rx->setWeakSignalProc(on); }
    LOGI("weak-signal processing: %d", on);
}
void LocalSdrShim::setNoiseBlanker(bool on) {
    g_dsp.nbOn.store(on);
    if (!p) return;
    p->nbOn.store(on);
    p->rx.setNoiseBlanker(on);
    { std::lock_guard<std::mutex> lk(p->clientMtx);
      for (auto& kv : p->clientDsp) if (kv.second && kv.second->rx) kv.second->rx->setNoiseBlanker(on); }
    LOGI("noise blanker: %d", on);
}
void LocalSdrShim::setCeq(bool on) {
    g_dsp.ceqOn.store(on);
    if (!p) return;
    p->ceqOn.store(on);
    p->rx.setCeq(on);
    { std::lock_guard<std::mutex> lk(p->clientMtx);
      for (auto& kv : p->clientDsp) if (kv.second && kv.second->rx) kv.second->rx->setCeq(on); }
    LOGI("CEQ (channel equaliser): %d", on);
}
void LocalSdrShim::setIms(bool on) {
    g_dsp.imsOn.store(on);
    if (!p) return;
    p->imsOn.store(on);
    p->rx.setIms(on);
    { std::lock_guard<std::mutex> lk(p->clientMtx);
      for (auto& kv : p->clientDsp) if (kv.second && kv.second->rx) kv.second->rx->setIms(on); }
    LOGI("IMS (adaptive IF): %d", on);
}
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
// ★★★ THIS IS THE OWNER'S RATE, NOT A LISTENER'S. It sets the server default — what the radio
//     runs at with nobody asking for anything else, and the floor it returns to when the last
//     listener that asked for less goes away.
//     A LISTENER asking to be slowed does NOT come here; it lands in `clientFps` and is applied
//     per listener on the way out. Routing a client's request through this function is the bug
//     this whole path exists to prevent: it is one number for the whole radio, so one idle phone
//     slowed everybody's waterfall and left it slow after disconnecting. If you are about to call
//     setFftRate() from a per-client code path, you want clientFps instead.
void LocalSdrShim::setFftRate(double fps) {
    if (!p || fps <= 0) return;
    double mr = g_vsMaxFftRate.load();       // never exceed the server's own cap
    if (mr > 0 && fps > mr) fps = mr;
    p->baseFftRate = fps;
    // Takes effect through the same path as a listener's request: the engine ends up at the
    // fastest rate anyone wants, which with nobody asking is exactly this.
    p->recomputeEngineRate();
    // ★ With listeners already attached and asking for MORE than this, recomputeEngineRate() will
    //   correctly decline to slow the engine — but the new default must still be recorded, which
    //   it is above. And if it declines because the rate is unchanged, the engine is already here.
    if (std::fabs(p->fftRate - fps) > 0.01) return;
    LOGI("fft rate: %.1f fps (engine %.1f) — server default", fps, fps * FFT_AVG);
}
bool LocalSdrShim::isAirspyHf() const { return p && p->useAirspyHf(); }

void LocalSdrShim::setSampleRate(double rate) {
    if (!p || rate <= 0) return;
    // ★★★ NO VIBE_HW_LOCK HERE, DELIBERATELY. This function calls stopDspThread() before it
    //     takes modeMtx, and that join waits on a thread which takes modeMtx per buffer —
    //     holding it from the top would deadlock exactly as the stopDspThread note warns.
    //     It takes the lock further down, at the only point where that is safe.
    if (p->radioReleased.load()) return;   // the radio is lent to another program
    // ★★★ A LOCKED CENTRE LOCKS THE RATE TOO. The centre and the rate TOGETHER define the
    //     captured window; pinning one and leaving the other client-changeable is incoherent,
    //     and it showed: with the centre held at 6.5 MHz a listener changed the rate and the
    //     whole display went misaligned (Stuart, 2026-08-02). Refused here, at the source, so
    //     that every caller is covered rather than just the one message handler.
    if (g_vsLockedCentre.load() > 0.0) {
        LOGI("sampleRate ignored — this receiver's window is LOCKED (centre %.3f MHz)",
             g_vsLockedCentre.load() / 1e6);
        return;
    }
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
    // ★★★ SAME SELF-DEADLOCK as the one fixed in spyRetuneDecimation — see the note there.
    //     sendConfig -> binsFor re-locks clientMtx, and this held it across the call. THIS is
    //     the one that fired: a listener changing the capture rate wedged the entire server.
    std::shared_ptr<net::Socket> scfg;
    { std::lock_guard<std::mutex> lk(impl->clientMtx); scfg = impl->specClient; }
    if (scfg) impl->sendConfig(scfg);
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

/** ★ How many RF gain POSITIONS this RSP offers (0 = not an RSP, or nothing attached). Published
 *  so the setup page can draw a slider over the real range instead of a box: the count is a
 *  property of the MODEL, and the model is something we know and the owner should not have to. */
int LocalSdrShim::rfGainPositions() const {
    if (!p || !p->useSdrplay() || !p->sdrp) return 0;
    return p->sdrp->lnaStateCount();
}

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
        // ★★★ THE SAME ONE RATE AS supportedRates(). This is the SECOND list — the apps read this
        //     one, the setup page reads the other — and fixing only one would leave the phone
        //     still offering spans that misalign the spectrum. The file says it plainly a few
        //     hundred lines up: "the hwinfo rates list, radioCapsJson and resumeCaptureIdle. NAME
        //     EVERY SOURCE." I fixed one of the three in 3.0.0-5 and this was still wrong.
        j += ",\"rates\":[";
        const auto& rl = a.sampleRates();
        if (!rl.empty()) j += std::to_string(*std::max_element(rl.begin(), rl.end()));
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
    // ★★ AND WHETHER IT IS ON. Only the CAPABILITY was ever published, so a receiver could sit
    //    there with DC on its feedline and no screen anywhere would say so — which is exactly how
    //    one did (2026-08-08). A control you cannot read the state of is not a control.
    j += std::string(",\"biasTOn\":") + (g_biasTeeOn.load() ? "true" : "false");
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
void LocalSdrShim::setBiasT(bool v)         { g_dsp.rspBiasT.store(v ? 1 : 0);
                                              if (!p || !p->useSdrplay()) return;
                                              VIBE_HW_LOCK(); p->sdrp->setBiasT(v); }
#undef VIBE_HW_LOCK

bool LocalSdrShim::isRunning() const { return p != nullptr; }

} // namespace vibe
