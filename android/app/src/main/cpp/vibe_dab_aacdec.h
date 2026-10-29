// vibe_dab_aacdec.h — DAB+ AAC decoded ON THE SERVER: FAAD2 in process on the Linux and Mac servers, the operating
// system's decoder everywhere else.
//
// ★★★ PER PLATFORM (V12, 2026-10-29 — Stuart: two libraries for AAC on the servers, FAAD2 and libxaac):
//   • Linux and Mac VibeServer: FAAD2's plain build, prefixed vibedab_ (vibe_dab_faad.h, VIBE_HAVE_FAAD2_DAB), on its
//     own thread. The ONLY DAB+ decoder there — no choice, no child process, no installed program needed. The release
//     builds refuse to configure without libfaad_dab.a (CMakeLists.txt), so a packaged server always has it.
//   • Android: AMediaCodec, the phone's own decoder ("Android server can use system hardware decoders").
//   • The iOS / Mac Catalyst app (local hardware) and a Mac dev build without the deps: AudioToolbox. ★ The App Store
//     archive never links FAAD2 (build_ios.sh checks).
//   • A Linux dev build without FAAD2: no decoder — available() says so and the DAB+ services are marked unplayable.
//
// ★★★ WHY DECODE ON THE SERVER AT ALL. Everything in the browser audio path — WebCodecs
//     probing, MediaSource fallbacks, 960-vs-1024 framing, parametric-stereo signalling, mp4
//     timescale arithmetic — exists ONLY for browsers, and Safari still is not right: DAB+ plays
//     "split seconds of audio" there. And the APPS have not been touched, so doing this
//     client-side means a second and third implementation with the same class of bugs. Decoding
//     here makes DAB+ exactly what MP2 already is: PCM, resampled to 48 kHz stereo, out through
//     the ordinary audio path. Format 4 leaves the wire and every client just works.
//
// ★★★ WHEN THERE IS NO DECODER, DAB DOES NOT DISAPPEAR. A server with no usable decoder still
//     shows the ensemble, still plays the MP2 services, and still carries the signal figures and
//     transmitter information DXers want; only the DAB+ services are marked unplayable. That is
//     Stuart's design and it is why available() is a question the caller can ask rather than a
//     hard requirement.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#elif defined(__APPLE__) && !defined(VIBE_HAVE_FAAD2_DAB)
#include <AudioToolbox/AudioToolbox.h>   // ★ outside the namespace below — system headers must not be wrapped in it
#endif

namespace vibedab {

/** Decoded PCM as the platform handed it back: interleaved float, at the decoder's OWN rate and
 *  channel count. ★ NOT normalised here — the caller already owns the one resampler that puts
 *  MP2 on 48 kHz stereo, and two of them would be two things to get wrong. */
struct AacPcm {
    std::vector<float> interleaved;
    int rateHz   = 0;
    int channels = 0;
};

/** Is FAAD2's DAB+ build in this binary? (Every Linux and Mac server build; never Android or the App Store app.) */
inline constexpr bool dabFaad2Built() {
#if defined(VIBE_HAVE_FAAD2_DAB)
    return true;
#else
    return false;
#endif
}

#if defined(__ANDROID__)

/** ★★ ADTS IN, PCM OUT, VIA AMediaCodec. Fed the very ADTS frames that used to go on the wire —
 *  so the reframing in vibe_dab_aac.h is unchanged and still the single description of what a
 *  DAB+ access unit is. */
class AacDecoder {
public:
    /** ★★★ AMediaCodec DOES THE 960-SAMPLE TRANSFORM — every access unit comes back exactly as
     *  long as it should, so its claimed rate is the truth and nothing here needs measuring.
     *  ★ It is also ASYNCHRONOUS: the first units return nothing and later ones return bursts,
     *    so a rate MEASURED over its early output is wrong by construction. Gating the
     *    measurement on this flag is what stopped the Xcover's DAB+ ramping up at the start of
     *    every service (Stuart, 2026-09-07 evening: it "never needed it"). */
    static constexpr bool kExactFrames = true;
    bool exactFrames() const { return true; }
    bool canConceal() const { return false; }   ///< see AacDecoderFaad — the OS decoder is fed the good units only
    void conceal(int, AacPcm&) {}
    int unitFrames() const { return 0; }
    double cpuSeconds() const { return -1.0; }  ///< inside the OS's media service — not ours to measure
    bool async() const { return false; }               ///< decodes in the call (see AacDecoderFaadAsync for the other kind)
    uint64_t inFlightFrames() const { return 0; }
    uint64_t dropped() const { return 0; }
    uint32_t generation() const { return 0; }
    template <class F> void setSink(F&&) {}
    void stop() {}
    int64_t concealed() const { return 0; }
    const char* backend() const { return "AMediaCodec"; }
    AacDecoder() = default;
    ~AacDecoder() { close(); }
    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    bool available() const { return !failed_; }

    /** Feed one ADTS frame; append whatever PCM the decoder is ready to give back.
     *  ★ Returns false only when the decoder is unusable — a frame that produces no output yet is
     *    normal (AAC decoders run a frame or two behind, and HE-AAC more). */
    bool decode(const uint8_t* adts, size_t n, AacPcm& out) {
        if (failed_) return false;
        if (n < 7) return true;
        /* ★★★ THE HEADER DECIDES THE CODEC, EVERY UNIT. The codec was opened once, from the first
         *  header it saw, and kept whatever that said — so a unit whose header names another
         *  rate or channel layout (a service change, or a first super frame assembled from the
         *  previous service's frames) went into a codec configured for something else, which
         *  decodes nothing and says nothing. Reopen on any change; cheap, and it happens once. */
        {
            const int profile = ((adts[2] >> 6) & 0x03) + 1;
            const int sfIndex = (adts[2] >> 2) & 0x0F;
            const int chCfg   = ((adts[2] & 0x01) << 2) | ((adts[3] >> 6) & 0x03);
            if (codec_ && (profile != openedProfile_ || sfIndex != openedSf_ || chCfg != openedCh_)) {
                fprintf(stderr, "[DAB] AAC header changed (sf %d→%d, ch %d→%d) — reopening the decoder\n",
                        openedSf_, sfIndex, openedCh_, chCfg);
                close();
            }
        }
        if (!codec_ && !open(adts, n)) return false;

        /* ★★★ THE ADTS HEADER IS STRIPPED AND THE CONFIG COMES FROM csd-0 INSTEAD. MediaCodec can
         *  be told "is-adts", but support for it has varied across vendors and a silent refusal
         *  here looks exactly like a dead multiplex. An AudioSpecificConfig built from the same
         *  header fields is the path every Android decoder implements. */
        const size_t hdr = ((adts[1] & 0x01) ? 7u : 9u);   // protection_absent -> no CRC
        if (n <= hdr) return true;
        const uint8_t* payload = adts + hdr;
        const size_t   plen    = n - hdr;

        ssize_t ib = AMediaCodec_dequeueInputBuffer(codec_, 10000);
        if (ib < 0 && ib != AMEDIACODEC_INFO_TRY_AGAIN_LATER) { restart("dequeueInputBuffer", ib); return true; }
        if (ib >= 0) {
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getInputBuffer(codec_, size_t(ib), &cap);
            if (buf && cap >= plen) {
                std::memcpy(buf, payload, plen);
                AMediaCodec_queueInputBuffer(codec_, size_t(ib), 0, plen, pts_, 0);
                pts_ += 21333;                    // ~1024 samples at 48 kHz; monotonic is all it needs
            } else {
                AMediaCodec_queueInputBuffer(codec_, size_t(ib), 0, 0, pts_, 0);
            }
        }
        drain(out);
        return true;
    }

    /** How many times an ERROR CODE has restarted the codec (the `dab` report's aacRestarts). A codec that goes quiet
     *  WITHOUT one is the service's watchdog (vibe_dab_service.h kAacDryMax, reported as aacRebuilds). */
    int restarts() const { return restarts_; }

    /** ★ Pull whatever is ready without feeding anything — used to flush the decoder's own lag. */
    void drain(AacPcm& out) {
        if (!codec_) return;
        for (int guard = 0; guard < 16; ++guard) {
            AMediaCodecBufferInfo info{};
            const ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
            if (ob >= 0) {
                size_t cap = 0;
                const uint8_t* buf = AMediaCodec_getOutputBuffer(codec_, size_t(ob), &cap);
                if (buf && info.size > 0) {
                    /* ★ 16-bit signed PCM is what the AAC decoder emits. Converted to float here
                     *  so the caller's resampler sees exactly what MP2 hands it. */
                    const int16_t* s = reinterpret_cast<const int16_t*>(buf + info.offset);
                    const size_t   ns = size_t(info.size) / sizeof(int16_t);
                    out.interleaved.reserve(out.interleaved.size() + ns);
                    for (size_t i = 0; i < ns; ++i)
                        out.interleaved.push_back(float(s[i]) * (1.0f / 32768.0f));
                    out.rateHz   = rate_;
                    out.channels = ch_;
                }
                AMediaCodec_releaseOutputBuffer(codec_, size_t(ob), false);
                continue;
            }
            if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                /* ★★★ READ THE RATE BACK, NEVER ASSUME IT. DAB+ is HE-AAC: SBR doubles the
                 *  decoder's output rate against the core rate in the ADTS header, and parametric
                 *  stereo turns a MONO core into TWO channels. A decoder that says 48 kHz stereo
                 *  from a 24 kHz mono core is correct, and taking the header's word for it would
                 *  play everything at half speed in mono — which is exactly the chipmunk family of
                 *  bugs MP2 already had. */
                AMediaFormat* f = AMediaCodec_getOutputFormat(codec_);
                if (f) {
                    int32_t v = 0;
                    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0) rate_ = v;
                    if (AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0) ch_ = v;
                    AMediaFormat_delete(f);
                }
                continue;
            }
            if (ob == AMEDIACODEC_INFO_TRY_AGAIN_LATER || ob == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) break;
            /* ★★★ ANYTHING ELSE NEGATIVE IS AN ERROR, AND A MediaCodec IN ITS ERROR STATE STAYS THERE until it is
             *  stopped and started again. This used to `break` like "nothing waiting", so one corrupt access unit
             *  could leave the codec dead for the rest of the programme while decode() kept answering true. */
            restart("dequeueOutputBuffer", ob);
            return;
        }
    }

    void close() {
        if (codec_) { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr; }
        rate_ = 0; ch_ = 0; pts_ = 0;
    }

    /** ★ A service change must not carry the previous programme's decoder state across. */
    void reset() { close(); failed_ = false; }

    int rateHz()   const { return rate_; }
    int channels() const { return ch_; }

private:
    bool open(const uint8_t* adts, size_t n) {
        if (n < 7) return false;
        /* ADTS: profile in bits 6-7 of byte 2, sampling_frequency_index in bits 2-5,
         * channel_configuration split across bytes 2 and 3. */
        const int profile = ((adts[2] >> 6) & 0x03) + 1;             // 2 = AAC-LC
        const int sfIndex = (adts[2] >> 2) & 0x0F;
        const int chCfg   = ((adts[2] & 0x01) << 2) | ((adts[3] >> 6) & 0x03);
        static const int kRates[16] = { 96000,88200,64000,48000,44100,32000,24000,22050,
                                        16000,12000,11025,8000,7350,0,0,0 };
        const int coreRate = kRates[sfIndex];
        if (coreRate <= 0 || chCfg <= 0) return false;

        /* ★★★ AudioSpecificConfig — AND THE FRAME LENGTH FLAG, WHICH IS THE WHOLE BALLGAME.
         *  Layout: 5 bits audioObjectType, 4 bits samplingFrequencyIndex, 4 bits
         *  channelConfiguration, then GASpecificConfig — whose FIRST bit is frameLengthFlag:
         *  0 = 1024 samples per frame, 1 = 960.
         *
         *  ★★★ DAB+ IS 960, ALWAYS. TS 102 563 specifies the 960-sample transform, which is what
         *      makes a 120 ms super frame divide into a whole number of access units at every core
         *      rate (16 kHz: 2 x 60 ms; 24 kHz: 3 x 40 ms; 48 kHz: 6 x 20 ms). With 1024 none of
         *      them come out whole, which is the arithmetic tell that the standard cannot be using
         *      it.
         *
         *  ★★★ AND GETTING IT WRONG DOES NOT SOUND BROKEN — IT SOUNDS SLOW. Left at 0 the decoder
         *      returns 2048 output samples for an access unit that carries 1920, so we deliver
         *      6.7% more audio than real time. MEASURED on 11A before this flag was set:
         *          pcm frames 1922745 in 37.7 s = 51000 Hz against the 48000 required — 1.062x
         *      Stuart heard exactly that and described it exactly: "still slow in safari but the
         *      audio is clear". Every other number looked right — the decoder reported 32 kHz
         *      stereo, every access unit decoded, the byte rate was sane — because the fault is
         *      not in any of them.
         *
         *  ★★ THIS IS THE SAME TRAP THE BROWSER PATH HIT. The 960-vs-1024 framing cost days on the
         *     MediaSource side; moving the decode to the server does not escape it, it just moves
         *     WHERE the frame length has to be declared. Written down here so the third
         *     implementation does not have to rediscover it.
         *
         *  ★ Overridable, because a decoder that ignores the flag would need the other value and
         *    that is a fact about the platform, not about DAB: VIBE_DAB_AAC_FRAME=1024. */
        static const bool k960 = !(std::getenv("VIBE_DAB_AAC_FRAME")
                                   && std::atoi(std::getenv("VIBE_DAB_AAC_FRAME")) == 1024);
        const uint8_t csd[2] = {
            uint8_t((profile << 3) | ((sfIndex >> 1) & 0x07)),
            uint8_t(((sfIndex & 0x01) << 7) | ((chCfg & 0x0F) << 3) | (k960 ? 0x04 : 0x00))
        };

        AMediaFormat* fmt = AMediaFormat_new();
        if (!fmt) { failed_ = true; return false; }
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, coreRate);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, chCfg);
        AMediaFormat_setBuffer(fmt, "csd-0", csd, sizeof csd);

        codec_ = AMediaCodec_createDecoderByType("audio/mp4a-latm");
        if (!codec_) { AMediaFormat_delete(fmt); failed_ = true; return false; }
        if (AMediaCodec_configure(codec_, fmt, nullptr, nullptr, 0) != AMEDIA_OK
            || AMediaCodec_start(codec_) != AMEDIA_OK) {
            AMediaFormat_delete(fmt);
            AMediaCodec_delete(codec_); codec_ = nullptr;
            failed_ = true; return false;
        }
        AMediaFormat_delete(fmt);
        /* ★ Seeded from the CORE, then corrected the moment the decoder reports its real output
         *  format. SBR and PS both change it and only the decoder knows. */
        rate_ = coreRate; ch_ = chCfg;
        openedProfile_ = profile; openedSf_ = sfIndex; openedCh_ = chCfg;
        return true;
    }

    /** ★ An error code (2026-10-09, Stuart: "I wonder if a bad dab packet causes a decoder crash on the AAC side"):
     *  drop the codec; the next unit's header reopens it (decode → open) — at once, rather than after the service's
     *  2–3 s dry watchdog. Not `failed_`: a decoder that errored
     *  once is not unusable, and marking it so would turn a sub-second hiccup into ADTS for the rest of the service. */
    void restart(const char* why, ssize_t code) {
        ++restarts_;
        fprintf(stderr, "[DAB] AAC decoder error (%s, %zd) — restarting it (%d so far)\n", why, code, restarts_);
        close();
    }

    AMediaCodec* codec_ = nullptr;
    int      restarts_ = 0;
    int      openedProfile_ = 0, openedSf_ = -1, openedCh_ = -1;   ///< what the codec was opened for
    int      rate_ = 0, ch_ = 0;
    int64_t  pts_  = 0;
    bool     failed_ = false;
};

#elif defined(__APPLE__) && !defined(VIBE_HAVE_FAAD2_DAB)

/** ★★★ AudioToolbox — for an Apple build WITHOUT FAAD2: the iOS / Mac Catalyst app's local hardware (the App Store
 *  archive links no FAAD2) and a Mac dev build without the deps prefix. The Mac VibeServer release links FAAD2 and never
 *  compiles this. Stuart, 2026-09-08: "route the AAC for DAB+ through the MacOS decoder".
 *  ★ An AudioConverter fed one access unit at a time. The ADTS header gives the core rate and
 *    channel count; a core at 24 kHz or below is HE-AAC (SBR, and parametric stereo when the core
 *    is mono) and decodes to twice its rate in stereo; a 32 or 48 kHz core is plain AAC-LC.
 *  ★ Apple decodes 1024-sample frames, not DAB+'s 960, so kExactFrames is false and DabService
 *    measures the real rate. */
class AacDecoderApple {
public:
    static constexpr bool kExactFrames = false;
    bool exactFrames() const { return false; }
    bool canConceal() const { return false; }
    void conceal(int, AacPcm&) {}
    int unitFrames() const { return 0; }
    double cpuSeconds() const { return -1.0; }
    bool async() const { return false; }               ///< decodes in the call (see AacDecoderFaadAsync for the other kind)
    uint64_t inFlightFrames() const { return 0; }
    uint64_t dropped() const { return 0; }
    uint32_t generation() const { return 0; }
    template <class F> void setSink(F&&) {}
    void stop() {}
    int64_t concealed() const { return 0; }
    AacDecoderApple() = default;
    ~AacDecoderApple() { close(); }
    AacDecoderApple(const AacDecoderApple&) = delete;
    AacDecoderApple& operator=(const AacDecoderApple&) = delete;
    bool available() const { return !failed_; }
    const char* backend() const { return "AudioToolbox"; }
    int restarts() const { return 0; }   ///< error-code restarts: the AMediaCodec decoder's only

    bool decode(const uint8_t* adts, size_t n, AacPcm& out) {
        if (failed_ || !adts || n < 7) return false;
        if (adts[0] != 0xFF || (adts[1] & 0xF0) != 0xF0) return false;
        const bool protAbsent = (adts[1] & 0x01) != 0;
        const size_t hdr = protAbsent ? 7 : 9;
        const int sfi = (adts[2] >> 2) & 0x0F;
        const int ch  = ((adts[2] & 0x01) << 2) | (adts[3] >> 6);
        static const int kRates[16] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0 };
        const int core = kRates[sfi];
        if (!core || n <= hdr) return false;
        if (!conv_ || core != coreRate_ || ch != coreCh_) { close(); if (!open(core, ch)) { failed_ = true; return false; } }
        pkt_ = adts + hdr; pktLen_ = n - hdr; pktGiven_ = false;
        std::vector<float> buf(size_t(4096) * 2);
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2;
        abl.mBuffers[0].mDataByteSize   = UInt32(buf.size() * sizeof(float));
        abl.mBuffers[0].mData           = buf.data();
        UInt32 frames = 4096;
        const OSStatus st = AudioConverterFillComplexBuffer(conv_, inputCb, this, &frames, &abl, nullptr);
        if (st != noErr && st != kNoMoreInput) { ++errors_; lastErr_ = st; if (errors_ > 200) failed_ = true; return false; }
        if (frames) {
            out.rateHz = rate_; out.channels = 2;
            out.interleaved.insert(out.interleaved.end(), buf.begin(), buf.begin() + size_t(frames) * 2);
        }
        return true;
    }
    void drain(AacPcm&) {}
    void close() { if (conv_) { AudioConverterDispose(conv_); conv_ = nullptr; } }
    void reset() { close(); failed_ = false; errors_ = 0; }
    int rateHz()   const { return conv_ ? rate_ : 0; }
    int channels() const { return conv_ ? 2 : 0; }
    long lastError() const { return long(lastErr_); }

private:
    enum : OSStatus { kNoMoreInput = 'nomo' };   // our own: the converter has eaten this unit
    static OSStatus inputCb(AudioConverterRef, UInt32* nPackets, AudioBufferList* io, AudioStreamPacketDescription** desc, void* user) {
        AacDecoderApple* self = static_cast<AacDecoderApple*>(user);
        if (self->pktGiven_ || !self->pkt_) { *nPackets = 0; return kNoMoreInput; }
        self->pktGiven_ = true;
        io->mNumberBuffers = 1;
        io->mBuffers[0].mNumberChannels = UInt32(self->inCh_);
        io->mBuffers[0].mData           = const_cast<uint8_t*>(self->pkt_);
        io->mBuffers[0].mDataByteSize   = UInt32(self->pktLen_);
        self->desc_.mStartOffset = 0; self->desc_.mVariableFramesInPacket = 0; self->desc_.mDataByteSize = UInt32(self->pktLen_);
        if (desc) *desc = &self->desc_;
        *nPackets = 1;
        return noErr;
    }
    bool open(int core, int ch) {
        const bool he = core <= 24000;
        AudioStreamBasicDescription in{};
        /* ★ MEASURED on the 12B capture (Radio 1 Dance, HE-AAC v2, 16 kHz core), 2026-09-08:
         *  HE_V2 at 2048 frames per packet decodes; HE_V2 at 1024, HE at 2048 and LC at 1024 decode
         *  NOTHING. Even so a quarter of the units come back 'bada' — Apple's parser wants
         *  1024-sample frames and DAB+ carries 960. That is why the Mac server decodes with FAAD2. */
        in.mFormatID        = he ? kAudioFormatMPEG4AAC_HE_V2 : kAudioFormatMPEG4AAC;
        in.mSampleRate      = core;
        in.mChannelsPerFrame = UInt32(he ? 2 : (ch ? ch : 2));
        in.mFramesPerPacket = he ? 2048 : 1024;
        UInt32 sz = sizeof in;
        AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, nullptr, &sz, &in);
        AudioStreamBasicDescription o{};
        o.mFormatID         = kAudioFormatLinearPCM;
        o.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        o.mSampleRate       = he ? core * 2 : core;
        o.mChannelsPerFrame = 2;
        o.mBitsPerChannel   = 32;
        o.mBytesPerFrame    = 8;
        o.mFramesPerPacket  = 1;
        o.mBytesPerPacket   = 8;
        if (AudioConverterNew(&in, &o, &conv_) != noErr) { conv_ = nullptr; return false; }
        coreRate_ = core; coreCh_ = ch; inCh_ = int(in.mChannelsPerFrame); rate_ = int(o.mSampleRate);
        return true;
    }

    AudioConverterRef conv_ = nullptr;
    AudioStreamPacketDescription desc_{};
    const uint8_t* pkt_ = nullptr; size_t pktLen_ = 0; bool pktGiven_ = true;
    int coreRate_ = 0, coreCh_ = 0, inCh_ = 2, rate_ = 0;
    bool failed_ = false; int errors_ = 0; OSStatus lastErr_ = 0;
};

using AacDecoder = AacDecoderApple;

#endif

#if !defined(__ANDROID__) && defined(VIBE_HAVE_FAAD2_DAB)
}  // namespace vibedab
#include "vibe_dab_faad.h"
namespace vibedab {
/** ★★★ THE LINUX AND MAC SERVERS' DAB+ DECODER: FAAD2, the only one (V12, 2026-10-29). It is told DAB+'s real 960-sample
 *  frames, so the pitch and the output rate are exact and nothing is measured or re-paced (kExactFrames), and a lost
 *  unit is concealed rather than spliced (canConceal).
 *  ★★★ FAAD2 RUNS ON ITS OWN THREAD (vibe-faad2 — AacDecoderFaadAsync): its decode and conceal calls return at once
 *      and the PCM arrives through the sink.
 *  ★ VIBE_DAB_FAAD_INLINE=1: FAAD2 decodes in the call instead, on the DAB thread, exactly as before the codec had a
 *    thread of its own — the A/B for measuring what the thread moves off the demodulator (and a way back). */
class AacDecoder {
public:
    static constexpr bool kExactFrames = true;
    AacDecoder() = default;
    static bool inlineFaad() { static const bool v = [] { const char* e = std::getenv("VIBE_DAB_FAAD_INLINE"); return e && e[0] == '1'; }(); return v; }
    bool async() const { return !inlineFaad(); }
    uint64_t inFlightFrames() const { return async() ? faad_.inFlightFrames() : 0; }
    uint64_t dropped() const { return faad_.dropped(); }
    uint32_t generation() const { return faad_.generation(); }
    void setSink(AacDecoderFaadAsync::Sink s) { faad_.setSink(std::move(s)); }
    void stop() { faad_.stop(); }
    double cpuDecodeSeconds() const { return inlineFaad() ? sync_.cpuDecodeSeconds() : faad_.cpuDecodeSeconds(); }
    double cpuResampleSeconds() const { return inlineFaad() ? sync_.cpuResampleSeconds() : faad_.cpuResampleSeconds(); }
    bool exactFrames() const { return true; }
    bool canConceal() const { return true; }
    void conceal(int n, AacPcm& out) { if (inlineFaad()) sync_.conceal(n, out); else faad_.conceal(n, out); }
    int unitFrames() const { return inlineFaad() ? sync_.unitFrames() : faad_.unitFrames(); }
    double cpuSeconds() const { return inlineFaad() ? sync_.cpuSeconds() : faad_.cpuSeconds(); }
    bool available() const { return true; }
    const char* backend() const { return "FAAD2"; }
    int restarts() const { return inlineFaad() ? sync_.restarts() : faad_.restarts(); }
    bool decode(const uint8_t* adts, size_t n, AacPcm& out) { return inlineFaad() ? sync_.decode(adts, n, out) : faad_.decode(adts, n, out); }
    void drain(AacPcm&) {}
    void close() { faad_.close(); sync_.close(); }
    void reset() { faad_.reset(); sync_.reset(); }
    int rateHz()   const { return inlineFaad() ? sync_.rateHz()   : faad_.rateHz(); }
    int channels() const { return inlineFaad() ? sync_.channels() : faad_.channels(); }
    int64_t concealed() const { return inlineFaad() ? sync_.concealed() : faad_.concealed(); }
private:
    AacDecoderFaadAsync faad_;
    AacDecoderFaad sync_;      ///< only with VIBE_DAB_FAAD_INLINE=1
};
#endif

#if !defined(__ANDROID__) && !defined(VIBE_HAVE_FAAD2_DAB) && !defined(__APPLE__)

/** ★★ NO DAB+ DECODER IN THIS BUILD (a Linux dev build without libfaad_dab.a) — available() answers honestly and the
 *  DAB+ services are marked unplayable rather than silently producing nothing. The ensemble, the MP2 services and the
 *  signal figures are unaffected. Release builds cannot get here (CMakeLists.txt requires FAAD2). */
class AacDecoder {
public:
    static constexpr bool kExactFrames = true;
    bool exactFrames() const { return true; }
    bool canConceal() const { return false; }
    void conceal(int, AacPcm&) {}
    int unitFrames() const { return 0; }
    double cpuSeconds() const { return -1.0; }
    bool async() const { return false; }               ///< decodes in the call (see AacDecoderFaadAsync for the other kind)
    uint64_t inFlightFrames() const { return 0; }
    uint64_t dropped() const { return 0; }
    uint32_t generation() const { return 0; }
    template <class F> void setSink(F&&) {}
    void stop() {}
    int64_t concealed() const { return 0; }
    bool available() const { return false; }
    const char* backend() const { return "none"; }
    int restarts() const { return 0; }   ///< error-code restarts: the AMediaCodec decoder's only
    bool decode(const uint8_t*, size_t, AacPcm&) { return false; }
    void drain(AacPcm&) {}
    void close() {}
    void reset() {}
    int  rateHz()   const { return 0; }
    int  channels() const { return 0; }
};

#endif

}  // namespace vibedab
