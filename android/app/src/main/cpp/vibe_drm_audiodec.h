// vibe_drm_audiodec.h — DRM audio decoded BY THE SERVER'S OPERATING SYSTEM, never by us (the DAB+ posture of
// vibe_dab_aacdec.h, applied to DRM). Access units and their AudioSpecificConfig come from decoders/drm/drm_audio.h.
//
// ★★★ PER PLATFORM:
//   • Linux / macOS with FFmpeg: FFmpeg run as a process, fed LOAS (the transport that carries ANY AudioSpecificConfig),
//     asked for 48 kHz stereo float. xHE-AAC needs FFmpeg's native USAC decoder, which arrived in FFmpeg 7.1 (2024):
//     Debian 13 has it, Ubuntu 24.04 (6.1) and Raspberry Pi OS bookworm (5.1) do not — and the listener is TOLD so, by
//     name, rather than given silence (canDecode()).
//   • macOS: AudioToolbox has decoded USAC since macOS 10.15 (kAudioFormatMPEGD_USAC) and is preferred for xHE-AAC when
//     it accepts the configuration; FFmpeg is the fallback.
//   • Android: MediaCodec with the AudioSpecificConfig as csd-0 (audio/mp4a-latm, profile 42) — xHE-AAC from Android 10
//     on devices whose codec list has it. ★ Written to the NDK's documented API; not yet run on a device (docs/v12/DRM.md).
//   • DRM's AAC (ER AAC Scalable + HCR/VCB11, 960, reversed SBR — see drm_audio.h) is decodable by NONE of these; the
//     answer for an AAC station is the honest one.
#pragma once
#include "decoders/drm/drm_audio.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#endif
#endif

namespace vibe {

#if !defined(__ANDROID__)
/** ★ FFmpeg, found by its known homes first (a Finder-launched Mac has no Homebrew on $PATH — vibe_dab_aacdec.h). */
inline const char* drmFfmpegPath() {
    static const std::string path = [] {
        if (const char* e = getenv("VIBE_DRM_FFMPEG")) return std::string(e);
        for (const char* c : { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" })
            if (::access(c, X_OK) == 0) return std::string(c);
        return std::string("ffmpeg");
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

/** LOAS in, 48 kHz stereo float out, through an FFmpeg child (the vibe_dab_aacdec.h pattern: non-blocking pipes, SIGPIPE
 *  ignored, never a permanent failure). */
class DrmFfmpegChild {
public:
    ~DrmFfmpegChild() { close(); }
    bool running() const { return pid_ > 0; }
    bool write(const std::vector<uint8_t>& loas) {
        if (pid_ < 0 && !start()) return false;
        const ssize_t w = ::write(inFd_, loas.data(), loas.size());
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { close(); return false; }
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
        pid_ = -1; carry_.clear();
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
                     "-f", "loas", "-i", "pipe:0", "-f", "f32le", "-ar", "48000", "-ac", "2", "pipe:1", (char*)nullptr);
            ::_exit(127);
        }
        ::close(in[0]); ::close(out[1]);
        inFd_ = in[1]; outFd_ = out[0];
        ::fcntl(inFd_, F_SETFL, O_NONBLOCK); ::fcntl(outFd_, F_SETFL, O_NONBLOCK);
        pid_ = pid;
        return true;
    }
    pid_t pid_ = -1; int inFd_ = -1, outFd_ = -1;
    std::vector<uint8_t> carry_;
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

/** ★★ The one door the DRM listener uses: configure for a service, ask whether it can be played (and why not), feed
 *  access units, collect PCM (48 kHz stereo on FFmpeg/Apple; the codec's own rate on Android — rateHz()). */
class DrmAudioDecoder {
public:
    /** Is this service's audio decodable on this server? `why` is a sentence for the listener when it is not. */
    static bool canDecode(const vibedrm::AudioInfo& a, std::string& why, std::string& backend) {
        backend.clear();
        if (!a.present) { why = "Waiting for the station's audio description."; return false; }
        if (a.coding == 0) {
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
        if (!c.present) why = "This station uses xHE-AAC. This server has no FFmpeg, so the audio cannot be played; the signal is "
                              "being received and decoded. (Installing FFmpeg 7.1 or newer adds it.)";
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
        const vibedrm::UsacInfo& u = fr.usac();
        if (!u.ok) { why_ = "This station's xHE-AAC configuration could not be read (" + u.why + ")."; return false; }
        usac_ = u; rate_ = a.sampleRateHz(); ch_ = a.audioMode == 2 ? 2 : 1;
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
#if defined(__ANDROID__)
        mc_.decode(au.data.data(), au.data.size(), pcm);
#else
#if defined(__APPLE__)
        if (at_.ok()) { at_.decode(au.data.data(), au.data.size(), pcm); return; }
#endif
        ff_.write(vibedrm::loasFrame(usac_.asc, usac_.ascBits, au.data.data(), au.data.size()));
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
    void close() {
        open_ = false;
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
#if defined(__ANDROID__)
    DrmMediaCodecUsac mc_;
#else
    DrmFfmpegChild ff_;
#if defined(__APPLE__)
    DrmAppleUsac at_;
#endif
#endif
};

}  // namespace vibe
