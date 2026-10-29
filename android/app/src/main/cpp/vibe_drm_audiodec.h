// vibe_drm_audiodec.h — DRM audio decoded BY THE SERVER'S OPERATING SYSTEM, never by us (the DAB+ posture of
// vibe_dab_aacdec.h, applied to DRM). Access units and their AudioSpecificConfig come from decoders/drm/drm_audio.h.
//
// ★★★ PER PLATFORM:
//   • Linux / macOS with FFmpeg: FFmpeg run as a process, fed a live Matroska stream (drm_audio.h MkvStream: the
//     AudioSpecificConfig as CodecPrivate — FFmpeg's LOAS reader cannot take a USAC config, found 2026-10-29), asked for
//     48 kHz stereo float. xHE-AAC needs FFmpeg's native USAC decoder (7.1+). ★★ THE LINUX PACKAGES BUNDLE FFmpeg 8.1
//     (V12: /usr/lib/vibeserver/ffmpeg, vibe_ffmpeg_path.h) because most distributions' is older (Ubuntu 24.04 6.1,
//     Raspberry Pi OS bookworm 5.1); a missing or old one is still TOLD to the listener, by name (canDecode()).
//   • DRM's AAC (all server builds with FAAD2 — VIBE_HAVE_FAAD2): FAAD2's DRM build, in process. That is the DEFAULT and
//     only route for DRM AAC; FFmpeg is used only for xHE-AAC.
//   • macOS: AudioToolbox has decoded USAC since macOS 10.15 (kAudioFormatMPEGD_USAC) and is preferred for xHE-AAC when
//     it accepts the configuration; FFmpeg is the fallback.
//   • Android: MediaCodec with the AudioSpecificConfig as csd-0 (audio/mp4a-latm, profile 42) — xHE-AAC from Android 10
//     on devices whose codec list has it. ★ Written to the NDK's documented API; not yet run on a device (docs/v12/DRM.md).
//   • DRM's AAC (ER AAC Scalable + HCR/VCB11, 960, reversed SBR — see drm_audio.h) is decodable by NONE of these; the
//     answer for an AAC station is the honest one.
#pragma once
#include "decoders/drm/drm_audio.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(VIBE_HAVE_FAAD2)
#include <neaacdec.h>   // ★★ FAAD2 (GPL-2.0-or-later), its DRM build — SERVER BUILDS ONLY (VIBE_DRM_FAAD2); never the App Store app
#endif
#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#else
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vibe_ffmpeg_path.h"   // ★ which ffmpeg: the package's own on Linux (V12), shared with DAB+
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif
#endif

namespace vibe {

#if !defined(__ANDROID__)
/** ★ FFmpeg — vibe_ffmpeg_path.h: the package's own on Linux (/usr/lib/vibeserver/ffmpeg, FFmpeg 8.1 with the xHE-AAC
 *  decoder), a system one only if that is missing; Homebrew's on a Mac. VIBE_DRM_FFMPEG still overrides it (tests). */
inline const char* drmFfmpegPath() {
    static const std::string path = [] {
        if (const char* e = getenv("VIBE_DRM_FFMPEG")) return std::string(e);
        return std::string(vibe::ffmpegPath());
    }();
    return path.c_str();
}
/** ★ What this machine's FFmpeg can do, probed ONCE: present, has the AAC decoder, and its version (USAC from 7.1). */
struct FfmpegCaps { bool present = false; bool aac = false; int major = 0, minor = 0; bool usac = false; std::string version; };
inline const FfmpegCaps& drmFfmpegCaps() {
    static const FfmpegCaps caps = [] {
        FfmpegCaps c;
        const std::string cmd = std::string("'") + drmFfmpegPath() + "' -hide_banner -version 2>/dev/null";
        if (FILE* p = ::popen(cmd.c_str(), "r")) {
            char line[512];
            if (std::fgets(line, sizeof line, p)) {
                // "ffmpeg version 7.1.1-1+b1 …", "ffmpeg version n7.1 …", or a git build "N-11xxxx-g…"
                const char* v = std::strstr(line, "version ");
                if (v) {
                    v += 8; c.version.assign(v, std::strcspn(v, " \n"));
                    const char* d = v; while (*d && (*d < '0' || *d > '9')) ++d;
                    if (*v == 'N' && v[1] == '-') { c.major = 99; c.minor = 0; }   // ★ a git snapshot: newer than any release
                    else if (*d) { c.major = std::atoi(d); const char* dot = std::strchr(d, '.'); c.minor = dot ? std::atoi(dot + 1) : 0; }
                    c.present = true;
                }
            }
            ::pclose(p);
        }
        if (c.present) {
            const std::string cmd2 = std::string("'") + drmFfmpegPath() + "' -hide_banner -decoders 2>/dev/null";
            if (FILE* p = ::popen(cmd2.c_str(), "r")) {
                char line[512];
                while (std::fgets(line, sizeof line, p)) if (std::strstr(line, " aac ")) { c.aac = true; break; }
                ::pclose(p);
            }
        }
        c.usac = c.aac && (c.major > 7 || (c.major == 7 && c.minor >= 1));
        return c;
    }();
    return caps;
}

/** Matroska in (drm_audio.h MkvStream), 48 kHz stereo float out, through an FFmpeg child (the vibe_dab_aacdec.h pattern:
 *  non-blocking pipes, SIGPIPE ignored, never a permanent failure). */
class DrmFfmpegChild {
public:
    ~DrmFfmpegChild() { close(); }
    bool running() const { return pid_ > 0; }
    /** The stream head every new child is given first (a restarted child starts a fresh stream). */
    void setHeader(std::vector<uint8_t> h) { head_ = std::move(h); }
    /** ★★ A CONTAINER MUST NOT LOSE BYTES. LOAS resynchronised on its next sync word; Matroska would read a half-written
     *  block as garbage. So what the pipe does not take now is kept and sent first next time — and if FFmpeg has fallen
     *  so far behind that a second of it is queued, the child is restarted rather than the queue grown. */
    bool write(const std::vector<uint8_t>& bytes) {
        if (pid_ < 0) { if (!start()) return false; pend_ = head_; }
        pend_.insert(pend_.end(), bytes.begin(), bytes.end());
        while (!pend_.empty()) {
            const ssize_t w = ::write(inFd_, pend_.data(), pend_.size());
            if (w > 0) { pend_.erase(pend_.begin(), pend_.begin() + w); continue; }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            close(); return false;
        }
        if (pend_.size() > 256 * 1024) { close(); return false; }
        return true;
    }
    void drain(std::vector<float>& out) {
        if (pid_ < 0) return;
        for (;;) {
            uint8_t buf[16384];
            const ssize_t r = ::read(outFd_, buf, sizeof buf);
            if (r <= 0) break;
            // ★ the pipe may split a float: keep the odd bytes for next time
            carry_.insert(carry_.end(), buf, buf + r);
            const size_t nf = carry_.size() / 4;
            const float* f = reinterpret_cast<const float*>(carry_.data());
            out.insert(out.end(), f, f + nf);
            carry_.erase(carry_.begin(), carry_.begin() + long(nf * 4));
            if (size_t(r) < sizeof buf) break;
        }
    }
    void close() {
        if (inFd_ >= 0) { ::close(inFd_); inFd_ = -1; }
        if (outFd_ >= 0) { ::close(outFd_); outFd_ = -1; }
        if (pid_ > 0) { ::kill(pid_, SIGKILL); int st = 0; ::waitpid(pid_, &st, 0); }
        pid_ = -1; carry_.clear(); pend_.clear();
    }
private:
    bool start() {
        static const bool ign = [] { ::signal(SIGPIPE, SIG_IGN); return true; }(); (void)ign;
        int in[2], out[2];
        if (::pipe(in) != 0) return false;
        if (::pipe(out) != 0) { ::close(in[0]); ::close(in[1]); return false; }
        const pid_t pid = ::fork();
        if (pid < 0) { ::close(in[0]); ::close(in[1]); ::close(out[0]); ::close(out[1]); return false; }
        if (pid == 0) {
            ::dup2(in[0], STDIN_FILENO); ::dup2(out[1], STDOUT_FILENO);
            ::close(in[1]); ::close(out[0]);
            const int dn = ::open("/dev/null", O_WRONLY); if (dn >= 0) ::dup2(dn, STDERR_FILENO);
            ::execlp(drmFfmpegPath(), "ffmpeg", "-hide_banner", "-loglevel", "quiet", "-nostdin",
                     "-fflags", "nobuffer", "-flags", "low_delay", "-probesize", "32", "-analyzeduration", "0",
                     "-f", "matroska", "-i", "pipe:0", "-f", "f32le", "-ar", "48000", "-ac", "2", "pipe:1", (char*)nullptr);
            ::_exit(127);
        }
        ::close(in[0]); ::close(out[1]);
        inFd_ = in[1]; outFd_ = out[0];
        ::fcntl(inFd_, F_SETFL, O_NONBLOCK); ::fcntl(outFd_, F_SETFL, O_NONBLOCK);
        pid_ = pid;
        return true;
    }
    pid_t pid_ = -1; int inFd_ = -1, outFd_ = -1;
    std::vector<uint8_t> carry_, head_, pend_;
};
#endif

#if defined(__APPLE__)
/** ★ AudioToolbox's USAC decoder, one access unit at a time; 48 kHz stereo float is asked for directly. */
class DrmAppleUsac {
public:
    ~DrmAppleUsac() { close(); }
    bool open(const vibedrm::UsacInfo& u, int rateHz, int channels) {
        close();
        AudioStreamBasicDescription in{}; in.mFormatID = kAudioFormatMPEGD_USAC; in.mSampleRate = rateHz;
        in.mChannelsPerFrame = UInt32(channels); in.mFramesPerPacket = UInt32(u.outputFrameLength);
        AudioStreamBasicDescription o{}; o.mFormatID = kAudioFormatLinearPCM; o.mSampleRate = 48000;
        o.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked; o.mChannelsPerFrame = 2;
        o.mBitsPerChannel = 32; o.mBytesPerFrame = 8; o.mFramesPerPacket = 1; o.mBytesPerPacket = 8;
        if (AudioConverterNew(&in, &o, &conv_) != noErr) { conv_ = nullptr; return false; }
        if (AudioConverterSetProperty(conv_, kAudioConverterDecompressionMagicCookie, UInt32(u.asc.size()), u.asc.data()) != noErr) {
            close(); return false;
        }
        frameLen_ = u.outputFrameLength; rate_ = rateHz;
        return true;
    }
    bool ok() const { return conv_ != nullptr; }
    void decode(const uint8_t* au, size_t n, std::vector<float>& out) {
        if (!conv_) return;
        pkt_.assign(au, au + n); fed_ = false;
        const UInt32 maxFrames = UInt32(std::max(4096, int(double(frameLen_) * 48000.0 / std::max(1, rate_)) + 64));
        std::vector<float> buf(size_t(maxFrames) * 2);
        AudioBufferList abl; abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2; abl.mBuffers[0].mDataByteSize = UInt32(buf.size() * 4); abl.mBuffers[0].mData = buf.data();
        UInt32 frames = maxFrames;
        const OSStatus st = AudioConverterFillComplexBuffer(conv_, inputCb, this, &frames, &abl, nullptr);
        if ((st == noErr || st == kNoMoreData) && frames > 0) out.insert(out.end(), buf.begin(), buf.begin() + long(frames) * 2);
    }
    void close() { if (conv_) { AudioConverterDispose(conv_); conv_ = nullptr; } }
private:
    static constexpr OSStatus kNoMoreData = 'nomd';
    static OSStatus inputCb(AudioConverterRef, UInt32* nPk, AudioBufferList* io, AudioStreamPacketDescription** desc, void* user) {
        DrmAppleUsac* s = static_cast<DrmAppleUsac*>(user);
        if (s->fed_) { *nPk = 0; return kNoMoreData; }
        s->fed_ = true;
        io->mNumberBuffers = 1; io->mBuffers[0].mData = s->pkt_.data(); io->mBuffers[0].mDataByteSize = UInt32(s->pkt_.size());
        io->mBuffers[0].mNumberChannels = 0;
        s->desc_.mStartOffset = 0; s->desc_.mVariableFramesInPacket = 0; s->desc_.mDataByteSize = UInt32(s->pkt_.size());
        if (desc) *desc = &s->desc_;
        *nPk = 1;
        return noErr;
    }
    AudioConverterRef conv_ = nullptr;
    std::vector<uint8_t> pkt_; bool fed_ = false; AudioStreamPacketDescription desc_{};
    int frameLen_ = 2048, rate_ = 48000;
};
#endif

#if defined(__ANDROID__)
/** ★ MediaCodec, csd-0 = the AudioSpecificConfig; asked for nothing — Android hands back its own rate, which the caller
 *  resamples (the DAB+ Android path does the same). Mono output is widened to stereo here. */
class DrmMediaCodecUsac {
public:
    ~DrmMediaCodecUsac() { close(); }
    bool open(const vibedrm::UsacInfo& u, int rateHz, int channels) {
        close();
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, rateHz);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_AAC_PROFILE, 42);
        AMediaFormat_setBuffer(f, "csd-0", u.asc.data(), u.asc.size());
        codec_ = AMediaCodec_createDecoderByType("audio/mp4a-latm");
        if (!codec_ || AMediaCodec_configure(codec_, f, nullptr, nullptr, 0) != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
            AMediaFormat_delete(f); close(); return false;
        }
        AMediaFormat_delete(f);
        outRate_ = rateHz; outCh_ = channels;
        return true;
    }
    bool ok() const { return codec_ != nullptr; }
    int rateHz() const { return outRate_; }
    /** Interleaved STEREO at rateHz(). */
    void decode(const uint8_t* au, size_t n, std::vector<float>& out) {
        if (!codec_) return;
        const ssize_t ib = AMediaCodec_dequeueInputBuffer(codec_, 5000);
        if (ib >= 0) {
            size_t cap = 0; uint8_t* b = AMediaCodec_getInputBuffer(codec_, size_t(ib), &cap);
            const size_t m = b && cap >= n ? n : 0;
            if (m) std::memcpy(b, au, m);
            AMediaCodec_queueInputBuffer(codec_, size_t(ib), 0, m, pts_, 0); pts_ += 42666;
        }
        for (int g = 0; g < 8; ++g) {
            AMediaCodecBufferInfo info{};
            const ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
            if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat* of = AMediaCodec_getOutputFormat(codec_);
                int32_t r = 0, c = 0;
                if (AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &r)) outRate_ = r;
                if (AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &c)) outCh_ = c;
                AMediaFormat_delete(of); continue;
            }
            if (ob < 0) break;
            size_t cap = 0; const uint8_t* b = AMediaCodec_getOutputBuffer(codec_, size_t(ob), &cap);
            const int16_t* s = reinterpret_cast<const int16_t*>(b + info.offset);
            const size_t ns = size_t(info.size) / 2;
            if (outCh_ == 1) for (size_t i = 0; i < ns; ++i) { const float v = s[i] / 32768.0f; out.push_back(v); out.push_back(v); }
            else for (size_t i = 0; i + 1 < ns; i += size_t(outCh_)) { out.push_back(s[i] / 32768.0f); out.push_back(s[i + 1] / 32768.0f); }
            AMediaCodec_releaseOutputBuffer(codec_, size_t(ob), false);
        }
    }
    void close() { if (codec_) { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr; } }
private:
    AMediaCodec* codec_ = nullptr; int64_t pts_ = 0; int outRate_ = 48000, outCh_ = 2;
};
#endif

/** ★ An integer-ratio upsampler to 48 kHz stereo (DRM's AAC comes out at 12, 24 or 48 kHz): a windowed-sinc polyphase
 *  FIR, 24 taps per phase, cut-off just under the input's Nyquist. */
class DrmUpsampler48 {
public:
    bool setRate(int inHz) {
        if (inHz <= 0 || 48000 % inHz) return false;
        if (inHz == in_) return true;
        in_ = inHz; L_ = 48000 / inHz; constexpr int kTaps = 24;
        const int n = kTaps * L_;
        h_.assign(size_t(n), 0.0f);
        const double fc = 0.45 / L_;   // cycles per OUTPUT sample
        for (int i = 0; i < n; ++i) {
            const double t = i - (n - 1) / 2.0;
            const double x = 2.0 * M_PI * fc * t;
            const double sinc = std::abs(t) < 1e-9 ? 2.0 * fc : std::sin(x) / (M_PI * t);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
            h_[size_t(i)] = float(sinc * w * L_);
        }
        hl_.assign(kTaps, 0.0f); hr_.assign(kTaps, 0.0f); pos_ = 0;
        return true;
    }
    /** Interleaved stereo in at the input rate → interleaved stereo out at 48 kHz, appended. */
    void process(const float* st, int frames, std::vector<float>& out) {
        const int T = int(hl_.size());
        for (int f = 0; f < frames; ++f) {
            hl_[size_t(pos_)] = st[2 * f]; hr_[size_t(pos_)] = st[2 * f + 1];
            pos_ = (pos_ + 1) % T;
            for (int ph = 0; ph < L_; ++ph) {
                float l = 0, r = 0;
                for (int k = 0; k < T; ++k) {
                    const int idx = (pos_ - 1 - k + 2 * T) % T;        // newest first
                    const float c = h_[size_t(k * L_ + ph)];
                    l += c * hl_[size_t(idx)]; r += c * hr_[size_t(idx)];
                }
                out.push_back(l); out.push_back(r);
            }
        }
    }
private:
    int in_ = 0, L_ = 1, pos_ = 0;
    std::vector<float> h_, hl_, hr_;
};

#if defined(VIBE_HAVE_FAAD2)
/** ★★★ DRM's AAC THROUGH FAAD2's DRM MODE (Stuart's decision, 2026-10-26: build FAAD2 into the SERVER so every client
 *  hears DRM AAC as ordinary audio). Nothing else free decodes it — docs/v12/DRM-AUDIO-ROUTES.md has the evidence.
 *  ★ FAAD2's DRM mode takes each frame with the super frame's CRC-8 IN FRONT of it and checks it (side information);
 *    SBR and PS are told by the channel code, from the SDC — the frames never say.
 *  ★★ A FRAME THAT FAILS IS CONCEALED, NOT GLITCHED: the last good frame is played again fading to silence, and the next
 *     good one fades in — a dropout on a fading HF signal sounds like a fade, not a click or a burst of garbage. */
class DrmFaad {
public:
    ~DrmFaad() { close(); }
    bool open(const vibedrm::AudioInfo& a) {
        close();
        h_ = NeAACDecOpen();
        if (!h_) return false;
        unsigned char ch = DRMCH_MONO;
        if (a.audioMode == 2) ch = a.sbr ? DRMCH_SBR_STEREO : DRMCH_STEREO;
        else if (a.audioMode == 1) ch = DRMCH_SBR_PS_STEREO;
        else ch = a.sbr ? DRMCH_SBR_MONO : DRMCH_MONO;
        if (NeAACDecInitDRM(&h_, (unsigned long)a.sampleRateHz(), ch) < 0) { close(); return false; }
        NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(h_);
        c->outputFormat = FAAD_FMT_FLOAT;
        NeAACDecSetConfiguration(h_, c);
        last_.clear(); fadeIn_ = false;
        return true;
    }
    bool ok() const { return h_ != nullptr; }
    void decode(const vibedrm::AccessUnit& au, std::vector<float>& out) {
        if (!h_) return;
        buf_.assign(1, au.crc);
        buf_.insert(buf_.end(), au.data.begin(), au.data.end());
        NeAACDecFrameInfo fi{};
        void* pcm = au.data.empty() ? nullptr : NeAACDecDecode(h_, &fi, buf_.data(), (unsigned long)buf_.size());
        std::vector<float> st;
        if (!pcm || fi.error || fi.samples == 0 || fi.channels == 0) {
            ++bad_;
            if (last_.empty()) return;
            // conceal: the last good frame again, fading out
            st = last_;
            const size_t n = st.size() / 2;
            for (size_t i = 0; i < n; ++i) { const float g = float(n - i) / float(n) * fadeGain_; st[2 * i] *= g; st[2 * i + 1] *= g; }
            fadeGain_ = 0.0f; fadeIn_ = true;
        } else {
            ++good_;
            const float* f = static_cast<const float*>(pcm);
            const size_t frames = fi.samples / fi.channels;
            st.resize(frames * 2);
            for (size_t i = 0; i < frames; ++i) {
                st[2 * i] = f[i * fi.channels];
                st[2 * i + 1] = f[i * fi.channels + (fi.channels > 1 ? 1 : 0)];
            }
            last_ = st; fadeGain_ = 1.0f;
            if (fadeIn_) {   // after a concealed frame: in again over ~10 ms
                const size_t n = std::min<size_t>(frames, size_t(fi.samplerate / 100));
                for (size_t i = 0; i < n; ++i) { const float g = float(i) / float(n); st[2 * i] *= g; st[2 * i + 1] *= g; }
                fadeIn_ = false;
            }
            rate_ = int(fi.samplerate);
        }
        if (rate_ > 0 && up_.setRate(rate_)) up_.process(st.data(), int(st.size() / 2), out);
    }
    void close() { if (h_) { NeAACDecClose(h_); h_ = nullptr; } }
    int64_t good() const { return good_; }
    int64_t bad() const { return bad_; }
    static std::string version() {
        char* id = nullptr; char* cp = nullptr;
        NeAACDecGetVersion(&id, &cp);
        return std::string("FAAD2 ") + (id ? id : "");
    }
private:
    NeAACDecHandle h_ = nullptr;
    std::vector<unsigned char> buf_;
    std::vector<float> last_; float fadeGain_ = 1.0f; bool fadeIn_ = false;
    int rate_ = 0; int64_t good_ = 0, bad_ = 0;
    DrmUpsampler48 up_;
};
#endif

/** ★★ The one door the DRM listener uses: configure for a service, ask whether it can be played (and why not), feed
 *  access units, collect PCM (48 kHz stereo on FFmpeg/Apple; the codec's own rate on Android — rateHz()). */
class DrmAudioDecoder {
public:
    /** Is this service's audio decodable on this server? `why` is a sentence for the listener when it is not. */
    static bool canDecode(const vibedrm::AudioInfo& a, std::string& why, std::string& backend) {
        backend.clear();
        if (!a.present) { why = "Waiting for the station's audio description."; return false; }
        if (a.coding == 0) {
#if defined(VIBE_HAVE_FAAD2)
            if (a.sampleRateHz() > 0) { backend = DrmFaad::version(); return true; }
#endif
            why = std::string("This station uses DRM's ") + a.codecName() +
                  " (error-resilient AAC). No decoder on this server's system can play it, so the audio cannot be played; "
                  "the signal, the station name and its text are still received and decoded.";
            return false;
        }
        if (a.coding != 3) { why = "This station uses an audio codec DRM reserves for the future."; return false; }
#if defined(__ANDROID__)
        backend = "MediaCodec";
        return true;   // ★ tried on open(); a refusal there turns into the sentence below
#else
#if defined(__APPLE__)
        if (!getenv("VIBE_DRM_NO_APPLE")) { backend = "AudioToolbox"; return true; }
#endif
        const FfmpegCaps& c = drmFfmpegCaps();
        if (c.usac) { backend = "ffmpeg " + c.version; return true; }
#if defined(__linux__)
        // ★ The package carries its own FFmpeg 8.1 (V12); missing means a damaged install, and reinstalling is the fix.
        if (!c.present) why = "This station uses xHE-AAC. This server's FFmpeg (/usr/lib/vibeserver/ffmpeg) is missing, so the audio "
                              "cannot be played; the signal is being received and decoded. (Reinstalling VibeServer restores it.)";
#else
        if (!c.present) why = "This station uses xHE-AAC. This server has no FFmpeg, so the audio cannot be played; the signal is "
                              "being received and decoded. (Installing FFmpeg 8.1 or newer adds it.)";
#endif
        else why = "This station uses xHE-AAC. This server's FFmpeg (" + c.version + ") has no xHE-AAC decoder — that arrived in "
                   "FFmpeg 7.1 — so the audio cannot be played; the signal is being received and decoded.";
        return false;
#endif
    }

    /** (Re)open for the framer's current configuration. */
    bool open(const vibedrm::AudioFramer& fr) {
        close();
        const vibedrm::AudioInfo& a = fr.info();
        if (!canDecode(a, why_, backend_)) return false;
#if defined(VIBE_HAVE_FAAD2)
        if (a.coding == 0) {
            if (!faad_.open(a)) { why_ = "FAAD2 refused this station's AAC configuration."; return false; }
            open_ = true; return true;
        }
#endif
        const vibedrm::UsacInfo& u = fr.usac();
        if (!u.ok) { why_ = "This station's xHE-AAC configuration could not be read (" + u.why + ")."; return false; }
        usac_ = u; rate_ = a.sampleRateHz(); ch_ = a.audioMode == 2 ? 2 : 1;
#if !defined(__ANDROID__)
        ff_.setHeader(mkv_.header(u.asc, rate_, ch_, u.outputFrameLength));   // ★ the config travels as CodecPrivate
#endif
#if defined(__ANDROID__)
        if (!mc_.open(u, rate_, ch_)) { why_ = "This station uses xHE-AAC, which this Android device's decoders do not offer."; return false; }
#else
#if defined(__APPLE__)
        if (backend_ == "AudioToolbox" && !at_.open(u, rate_, ch_)) {
            const FfmpegCaps& c = drmFfmpegCaps();
            if (c.usac) backend_ = "ffmpeg " + c.version;
            else { why_ = "This Mac refused the station's xHE-AAC configuration."; return false; }
        }
#endif
        /* ★★ FFmpeg (8.1, the bundled one) refuses USAC's eSBR tools outright — every unit would come back as an error and
         *  the listener would get silence with no reason. Say it instead. (Its LPD/ACELP speech core is not decoded
         *  either, but that is chosen frame by frame and cannot be known from the config.) */
        if (backend_.rfind("ffmpeg", 0) == 0 && u.esbr) {
            why_ = "This station uses xHE-AAC with enhanced SBR, which this server's FFmpeg (" + drmFfmpegCaps().version +
                   ") cannot decode yet, so the audio cannot be played; the signal is being received and decoded.";
            return false;
        }
#endif
        open_ = true;
        return true;
    }
    bool isOpen() const { return open_; }
    const std::string& why() const { return why_; }
    const std::string& backend() const { return backend_; }
    int rateHz() const {
#if defined(__ANDROID__)
        return mc_.rateHz();
#else
        return 48000;
#endif
    }
    /** One access unit in; whatever PCM is ready out (interleaved stereo). */
    void decode(const vibedrm::AccessUnit& au, std::vector<float>& pcm) {
        if (!open_) return;
#if defined(VIBE_HAVE_FAAD2)
        if (faad_.ok()) { faad_.decode(au, pcm); return; }
#endif
#if defined(__ANDROID__)
        mc_.decode(au.data.data(), au.data.size(), pcm);
#else
#if defined(__APPLE__)
        if (at_.ok()) { at_.decode(au.data.data(), au.data.size(), pcm); return; }
#endif
        if (!ff_.running()) mkv_.newCluster();   // ★ a new (or restarted) child gets the head, then a fresh Cluster
        ff_.write(mkv_.block(au.data.data(), au.data.size()));
        ff_.drain(pcm);
#endif
    }
    void drain(std::vector<float>& pcm) {
#if !defined(__ANDROID__)
        if (open_) ff_.drain(pcm);
#else
        (void)pcm;
#endif
    }
    /** DRM AAC frames decoded / concealed (FAAD2), for the report. */
    int64_t unitsGood() const {
#if defined(VIBE_HAVE_FAAD2)
        return faad_.good();
#else
        return 0;
#endif
    }
    int64_t unitsConcealed() const {
#if defined(VIBE_HAVE_FAAD2)
        return faad_.bad();
#else
        return 0;
#endif
    }
    void close() {
        open_ = false;
#if defined(VIBE_HAVE_FAAD2)
        faad_.close();
#endif
#if defined(__ANDROID__)
        mc_.close();
#else
        ff_.close();
#if defined(__APPLE__)
        at_.close();
#endif
#endif
    }
private:
    bool open_ = false; std::string why_, backend_;
    vibedrm::UsacInfo usac_; int rate_ = 48000, ch_ = 1;
#if defined(VIBE_HAVE_FAAD2)
    DrmFaad faad_;
#endif
#if defined(__ANDROID__)
    DrmMediaCodecUsac mc_;
#else
    DrmFfmpegChild ff_;
    vibedrm::MkvStream mkv_;
#if defined(__APPLE__)
    DrmAppleUsac at_;
#endif
#endif
};

}  // namespace vibe
