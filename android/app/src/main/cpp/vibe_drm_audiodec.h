// vibe_drm_audiodec.h — DRM audio decoded ON THE SERVER (the DAB+ posture of vibe_dab_aacdec.h, applied to DRM).
// Access units and their AudioSpecificConfig come from decoders/drm/drm_audio.h.
//
// ★★★ PER PLATFORM (V12, 2026-10-29 — the Linux and Mac servers decode AAC with exactly two libraries):
//   • DRM's AAC (all server builds with FAAD2 — VIBE_HAVE_FAAD2): FAAD2's DRM build, in process.
//   • xHE-AAC on every server build with libxaac (VIBE_HAVE_LIBXAAC — the Linux packages and the Mac VibeServer):
//     libxaac IN PROCESS (vibe_xaac.h), one access unit at a time — every USAC tool (SBR, eSBR, the ACELP/TCX speech
//     core, MPEG Surround 2-1-2); docs/v12/LIBXAAC.md.
//   • Android: MediaCodec with the AudioSpecificConfig as csd-0 (audio/mp4a-latm, profile 42) — xHE-AAC from Android 10
//     on devices whose codec list has it. ★ Written to the NDK's documented API; not yet run on a device (docs/v12/DRM.md).
//   • The iOS / Mac Catalyst app (no FAAD2, no libxaac — the App Store archive links no decoder library): AudioToolbox,
//     which has decoded USAC since macOS 10.15 (kAudioFormatMPEGD_USAC). DRM's own AAC is not decodable there.
//   • A Linux dev build without libxaac: xHE-AAC is not decodable, and the listener is TOLD so, by name (canDecode()).
//   • DRM's AAC (ER AAC Scalable + HCR/VCB11, 960, reversed SBR — see drm_audio.h) needs FAAD2's DRM build; where that
//     is missing, the answer for an AAC station is the honest one.
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

#include "vibe_xaac.h"   // ★★ libxaac (Apache-2.0) — xHE-AAC in process on server builds (VIBE_HAVE_LIBXAAC)
#if defined(VIBE_HAVE_FAAD2)
#include <neaacdec.h>   // ★★ FAAD2 (GPL-2.0-or-later), its DRM build — SERVER BUILDS ONLY (VIBE_DRM_FAAD2); never the App Store app
#endif
#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#elif defined(__APPLE__) && !defined(VIBE_HAVE_LIBXAAC)
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace vibe {

#if defined(__APPLE__) && !defined(VIBE_HAVE_LIBXAAC)
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
 *  access units, collect PCM (48 kHz stereo from FAAD2, libxaac and AudioToolbox; the codec's own rate on Android —
 *  rateHz()). */
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
#if defined(VIBE_HAVE_LIBXAAC)
        backend = XaacDecoder::version();   // ★ in process, every USAC tool (vibe_xaac.h)
        return true;
#elif defined(__ANDROID__)
        backend = "MediaCodec";
        return true;   // ★ tried on open(); a refusal there turns into the sentence below
#elif defined(__APPLE__)
        backend = "AudioToolbox";
        return true;   // ★ tried on open(); a refusal there turns into the sentence below
#else
        // ★ Only a Linux dev build without libxaac reaches this (the packages and the Mac app link libxaac).
        why = "This station uses xHE-AAC. This server was built without an xHE-AAC decoder, so the audio cannot be played; "
              "the signal is being received and decoded.";
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
#if defined(VIBE_HAVE_LIBXAAC)
        if (!xaac_.open(u.asc)) { why_ = "This station's xHE-AAC configuration was refused by the decoder (" + XaacDecoder::version() + ")."; return false; }
#elif defined(__ANDROID__)
        if (!mc_.open(u, rate_, ch_)) { why_ = "This station uses xHE-AAC, which this Android device's decoders do not offer."; return false; }
#elif defined(__APPLE__)
        if (!at_.open(u, rate_, ch_)) { why_ = "This device refused the station's xHE-AAC configuration."; return false; }
#else
        return false;   // ★ unreachable: canDecode() said no
#endif
        open_ = true;
        return true;
    }
    bool isOpen() const { return open_; }
    const std::string& why() const { return why_; }
    const std::string& backend() const { return backend_; }
    int rateHz() const {
#if defined(__ANDROID__) && !defined(VIBE_HAVE_LIBXAAC)
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
#if defined(VIBE_HAVE_LIBXAAC)
        if (xaac_.ok()) { xaac_.decode(au.data.data(), au.data.size(), au.crcOk, pcm); return; }
#elif defined(__ANDROID__)
        mc_.decode(au.data.data(), au.data.size(), pcm);
#elif defined(__APPLE__)
        if (at_.ok()) { at_.decode(au.data.data(), au.data.size(), pcm); return; }
#endif
    }
    void drain(std::vector<float>&) {}   // ★ every decoder here hands its PCM back in decode()
    /** Frames decoded / concealed (FAAD2 for DRM AAC, libxaac for xHE-AAC), for the report. */
    int64_t unitsGood() const {
        int64_t n = 0;
#if defined(VIBE_HAVE_FAAD2)
        n += faad_.good();
#endif
#if defined(VIBE_HAVE_LIBXAAC)
        n += xaac_.good();
#endif
        return n;
    }
    int64_t unitsConcealed() const {
        int64_t n = 0;
#if defined(VIBE_HAVE_FAAD2)
        n += faad_.bad();
#endif
#if defined(VIBE_HAVE_LIBXAAC)
        n += xaac_.bad();
#endif
        return n;
    }
    void close() {
        open_ = false;
#if defined(VIBE_HAVE_FAAD2)
        faad_.close();
#endif
#if defined(VIBE_HAVE_LIBXAAC)
        xaac_.close();
#elif defined(__ANDROID__)
        mc_.close();
#elif defined(__APPLE__)
        at_.close();
#endif
    }
private:
    bool open_ = false; std::string why_, backend_;
    vibedrm::UsacInfo usac_; int rate_ = 48000, ch_ = 1;
#if defined(VIBE_HAVE_FAAD2)
    DrmFaad faad_;
#endif
#if defined(VIBE_HAVE_LIBXAAC)
    XaacDecoder xaac_;
#elif defined(__ANDROID__)
    DrmMediaCodecUsac mc_;
#elif defined(__APPLE__)
    DrmAppleUsac at_;
#endif
};

}  // namespace vibe
