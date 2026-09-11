// vibe_dab_aacdec.h — DAB+ AAC decoded BY THE SERVER'S OPERATING SYSTEM, never by us.
//
// ★★★ WE STILL SHIP NO AAC DECODER, AND THAT IS DELIBERATE, NOT INCIDENTAL. vibe_dab_aac.h says
//     it "parses, error-corrects and REFRAMES ... it never produces PCM, and VibeServer links no
//     AAC decoder — the browser's own does the decoding, which it is already licensed for." That
//     position is unchanged here: this file calls the PLATFORM's decoder, the same posture as
//     relying on the browser's. Android's AMediaCodec is part of the OS. Nothing is bundled.
//
// ★★★ WHY MOVE IT OFF THE CLIENT AT ALL. Everything in the browser audio path — WebCodecs
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

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#elif defined(__unix__) || defined(__APPLE__)
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>   // ★ outside the namespace below — system headers must not be wrapped in it
#endif
#include <cerrno>
#include <cstdlib>
#include <cstring>          // ★ glibc does not pull this in transitively; macOS does
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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
            break;      // TRY_AGAIN_LATER or buffers changed — nothing waiting
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

    AMediaCodec* codec_ = nullptr;
    int      openedProfile_ = 0, openedSf_ = -1, openedCh_ = -1;   ///< what the codec was opened for
    int      rate_ = 0, ch_ = 0;
    int64_t  pts_  = 0;
    bool     failed_ = false;
};

#elif defined(__APPLE__)

/** ★★★ THE PLATFORM'S DECODER ON macOS IS AudioToolbox — the same posture as AMediaCodec on
 *  Android and ffmpeg on Linux: the operating system decodes, nothing is shipped. Stuart,
 *  2026-09-08: "route the AAC for DAB+ through the MacOS decoder".
 *  ★ An AudioConverter fed one access unit at a time. The ADTS header gives the core rate and
 *    channel count; a core at 24 kHz or below is HE-AAC (SBR, and parametric stereo when the core
 *    is mono) and decodes to twice its rate in stereo; a 32 or 48 kHz core is plain AAC-LC.
 *  ★ Apple decodes 1024-sample frames, not DAB+'s 960 — the same substitution ffmpeg makes, so
 *    kExactFrames is false and DabService measures the real rate exactly as it does for ffmpeg. */
class AacDecoderApple {
public:
    static constexpr bool kExactFrames = false;
    AacDecoderApple() = default;
    ~AacDecoderApple() { close(); }
    AacDecoderApple(const AacDecoderApple&) = delete;
    AacDecoderApple& operator=(const AacDecoderApple&) = delete;
    bool available() const { return !failed_; }
    const char* backend() const { return "AudioToolbox"; }

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
         *  1024-sample frames and DAB+ carries 960, and where ffmpeg is lenient this one refuses.
         *  That is why the wrapper below prefers ffmpeg when the machine has it. */
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

#endif
/* ★ NOT ANDROID — Bionic defines __unix__ too, and the AMediaCodec class above is Android's. */
#if !defined(__ANDROID__) && (defined(__unix__) || defined(__APPLE__))

/** ★★★ THE PLATFORM'S DECODER ON A POSIX SERVER IS ffmpeg, RUN AS A PROCESS.
 *  ★ On macOS too, when it is installed (Homebrew): see AacDecoder below for why it is preferred.
 *
 *  ★★★ STILL NOT A DECODER WE SHIP. Same posture as AMediaCodec above and as the browser's before
 *      that: we hand ADTS to something the operating system already provides and take PCM back.
 *      Nothing is linked, nothing is bundled, and a machine without ffmpeg simply reports
 *      available() == false. It is also what OpenWebRX does with dablin, so it is a shape this
 *      hardware is already known to sustain.
 *
 *  ★★★ ffmpeg RESAMPLES TO 48 kHz STEREO FOR US, AND THAT IS DELIBERATE. DAB+ is HE-AAC: the core
 *      rate in the ADTS header is not the output rate (SBR doubles it) and a mono core can decode
 *      to two channels (parametric stereo). Asking the decoder for exactly what the audio path
 *      wants removes that whole class of question — the same class that made the Android path
 *      play at the wrong speed until the output format was read back rather than assumed.
 *
 *  ★★ ONE PROCESS PER SERVICE, STARTED LAZILY AND KILLED ON A SERVICE CHANGE. A DAB+ service can
 *     change rate and channel mode, and a decoder configured for the previous one produces
 *     confident nonsense.
 *  ★ SIGPIPE is ignored once, process-wide: ffmpeg exiting mid-write must return an error here,
 *    not kill the server.
 */
class AacDecoderFfmpeg {
public:
    /** ★★★ THIS DECODER CANNOT DO THE 960-SAMPLE DAB+ TRANSFORM — it returns 1024 samples per
     *  access unit, so the caller must MEASURE its real output rate (see DabService). */
    static constexpr bool kExactFrames = false;
    AacDecoderFfmpeg() = default;
    ~AacDecoderFfmpeg() { close(); }
    AacDecoderFfmpeg(const AacDecoderFfmpeg&) = delete;
    AacDecoderFfmpeg& operator=(const AacDecoderFfmpeg&) = delete;
    const char* backend() const { return "ffmpeg"; }

    /** ★ Probed ONCE: the binary must exist AND report an AAC decoder. A server with ffmpeg built
     *  without AAC would otherwise look capable and deliver silence. The BINARY does not come and
     *  go, so probing once is right; the CHILD PROCESS does, which is what the retry below is for.
     *
     *  ★★★ `failed_` USED TO BE A ONE-WAY LATCH, AND IT KILLED DAB+ FOR THE LIFE OF THE SERVER.
     *      One broken pipe — an ffmpeg child killed by anything at all — set it, nothing ever
     *      cleared it, and from that moment every DAB+ access unit was dropped while the audio
     *      path gap-filled 48 kHz of nothing. Measured on the OWRX box (2026-09-11): superframes
     *      arriving perfectly (sfOk 67 of 67, Reed-Solomon 0 fixed 0 lost, MER 23.4 dB, IQ dropped
     *      0) with `aacDecoded` stuck at 0 and `pcmFilled` climbing ~48,000/s — the whole of the
     *      audio being filler. What the listener gets depends only on the filler: a 32 kHz service
     *      crackles constantly, a 48 kHz one goes silent (Stuart: "audio gone completely but look
     *      at the reception its the cleanest out of them all", then "on a 32K station constant
     *      crackle"). Both are this one fault, and from the outside both read as broken hardware —
     *      which is precisely why the receiver looked worst on the machine whose reception was best.
     *
     *  ★★ SO IT RETRIES, AND IT NEVER GIVES UP PERMANENTLY. The cooldown backs off to a cap so a
     *     genuinely broken ffmpeg cannot spin the box respawning it, but there is no attempt count
     *     that ends in a receiver that has quietly stopped decoding for good. A radio that fixes
     *     itself when the cause goes away is worth more than one that has to be restarted by hand,
     *     and "never limit permanently" is the rule this broke. */
    bool available() const {
        static const bool ok = probe();
        if (!ok) return false;
        if (!failed_) return true;
        const long cool = coolSec();
        if (::time(nullptr) - failedAt_ < cool) return false;
        failed_ = false;            // ★ due for another go — decode() will start a fresh child
        return true;
    }

    bool decode(const uint8_t* adts, size_t n, AacPcm& out) {
        if (!available() || n < 7) return available();
        if (pid_ < 0 && !start()) return false;
        ssize_t w = ::write(inFd_, adts, n);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { fail("write to ffmpeg failed"); return false; }
        drain(out);
        return true;
    }

    void drain(AacPcm& out) {
        if (pid_ < 0) return;
        for (;;) {
            uint8_t buf[16384];
            const ssize_t r = ::read(outFd_, buf, sizeof buf);
            if (r <= 0) break;
            const float* f = reinterpret_cast<const float*>(buf);
            const size_t nf = size_t(r) / sizeof(float);
            out.interleaved.insert(out.interleaved.end(), f, f + nf);
            out.rateHz = 48000; out.channels = 2;      // what we asked ffmpeg for
            if (size_t(r) < sizeof buf) break;
        }
    }

    void close() {
        if (inFd_  >= 0) { ::close(inFd_);  inFd_  = -1; }
        if (outFd_ >= 0) { ::close(outFd_); outFd_ = -1; }
        if (pid_ > 0) { ::kill(pid_, SIGKILL); int st = 0; ::waitpid(pid_, &st, 0); }
        pid_ = -1;
    }
    void reset() { close(); failed_ = false; failures_ = 0; }
    int rateHz()   const { return pid_ > 0 ? 48000 : 0; }
    int channels() const { return pid_ > 0 ? 2 : 0; }

private:
    /** ★★★ THE BINARY IS FOUND BY PATH, NOT BY $PATH. A macOS app launched from the Finder has
     *  /usr/bin:/bin:/usr/sbin:/sbin and nothing else — Homebrew's /opt/homebrew/bin (Apple
     *  silicon) and /usr/local/bin (Intel) are not on it, so `execlp("ffmpeg")` would report "no
     *  ffmpeg" on a Mac that has it. The known homes first, then whatever $PATH offers. */
    static const char* ffmpegPath() {
        static const std::string path = [] {
            for (const char* c : { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" })
                if (::access(c, X_OK) == 0) return std::string(c);
            return std::string("ffmpeg");
        }();
        return path.c_str();
    }
    static bool probe() {
        /* ★ `ffmpeg -decoders` and look for the aac line — the same check by hand as
         *  `ffmpeg -decoders | grep ' aac '`, which is how the Pi was confirmed: "A....D aac". */
        const std::string cmd = std::string("'") + ffmpegPath() + "' -hide_banner -decoders 2>/dev/null";
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) return false;
        char line[512]; bool found = false;
        while (std::fgets(line, sizeof line, p)) {
            const char* a = std::strstr(line, " aac ");
            if (a) { found = true; break; }
        }
        ::pclose(p);
        return found;
    }

    bool start() {
        static const bool ignoredPipe = [] { ::signal(SIGPIPE, SIG_IGN); return true; }();
        (void)ignoredPipe;
        int inPipe[2], outPipe[2];
        if (::pipe(inPipe) != 0) return false;
        if (::pipe(outPipe) != 0) { ::close(inPipe[0]); ::close(inPipe[1]); return false; }
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(inPipe[0]); ::close(inPipe[1]); ::close(outPipe[0]); ::close(outPipe[1]);
            fail("could not fork for ffmpeg"); return false;
        }
        if (pid == 0) {
            ::dup2(inPipe[0], STDIN_FILENO);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::close(inPipe[1]); ::close(outPipe[0]);
            const int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) ::dup2(devnull, STDERR_FILENO);
            /* ★ nobuffer + low_delay because this is a live stream, not a file: ffmpeg's default
             *  input buffering would add latency the audio path then has to carry for ever. */
            ::execlp(ffmpegPath(), "ffmpeg", "-hide_banner", "-loglevel", "quiet", "-nostdin",
                     "-fflags", "nobuffer", "-flags", "low_delay",
                     "-f", "aac", "-i", "pipe:0",
                     "-f", "f32le", "-ar", "48000", "-ac", "2", "pipe:1", (char*)nullptr);
            ::_exit(127);
        }
        ::close(inPipe[0]); ::close(outPipe[1]);
        inFd_ = inPipe[1]; outFd_ = outPipe[0];
        ::fcntl(inFd_,  F_SETFL, O_NONBLOCK);
        ::fcntl(outFd_, F_SETFL, O_NONBLOCK);
        pid_ = pid;
        return true;
    }

    /** ★ ONE PLACE THAT RECORDS A FAILURE, so the cooldown, the counter and the message cannot
     *  drift apart — and so the journal SAYS the decoder has gone. Silence in the log is what let
     *  this sit undiagnosed behind a perfect-looking receiver. */
    void fail(const char* why) {
        close();
        failed_ = true;
        failedAt_ = ::time(nullptr);
        if (failures_ < 1000000) ++failures_;
        std::fprintf(stderr, "[DAB] AAC decoder lost (%s) — DAB+ audio is filler until it restarts; "
                             "retrying in %lds (failure %d)\n", why, coolSec(), failures_);
        std::fflush(stderr);
    }
    /** ★ 2 s, doubling to a 30 s cap. Quick enough that a one-off death is inaudible; slow enough
     *  that a machine with no working ffmpeg is not forking one twice a second. */
    long coolSec() const {
        long c = 2; for (int i = 1; i < failures_ && c < 30; ++i) c *= 2;
        return c > 30 ? 30 : c;
    }

    pid_t pid_ = -1;
    int   inFd_ = -1, outFd_ = -1;
    mutable bool failed_ = false;
    mutable long failedAt_ = 0;
    int   failures_ = 0;
};

#if defined(__APPLE__)
/** ★★★ macOS: ffmpeg IF THE MACHINE HAS IT, AudioToolbox OTHERWISE. Stuart, 2026-09-08: "route
 *  the AAC for DAB+ through the MacOS decoder". Measured on the 12B capture, Apple's decoder
 *  refuses a quarter of DAB+'s 960-sample access units as bad data (it parses 1024-sample frames)
 *  where ffmpeg decodes every one of them — so a Mac with Homebrew ffmpeg gets the clean path and
 *  a Mac without it still plays DAB+ with the operating system's own decoder rather than not at
 *  all. Which one is in use is reported (backend()) so nobody has to guess from the sound. */
class AacDecoder {
public:
    static constexpr bool kExactFrames = false;
    /** ★ VIBE_AAC_APPLE=1 forces AudioToolbox — read once, here, so dab-offline can measure both. */
    AacDecoder() : useFf_(ff_.available() && !getenv("VIBE_AAC_APPLE")) {}
    bool available() const { return useFf_ ? ff_.available() : at_.available(); }
    const char* backend() const { return useFf_ ? ff_.backend() : at_.backend(); }
    bool decode(const uint8_t* adts, size_t n, AacPcm& out) { return useFf_ ? ff_.decode(adts, n, out) : at_.decode(adts, n, out); }
    void drain(AacPcm& out) { if (useFf_) ff_.drain(out); else at_.drain(out); }
    void close() { ff_.close(); at_.close(); }
    void reset() { ff_.reset(); at_.reset(); }
    int rateHz()   const { return useFf_ ? ff_.rateHz()   : at_.rateHz(); }
    int channels() const { return useFf_ ? ff_.channels() : at_.channels(); }
private:
    AacDecoderFfmpeg ff_;
    AacDecoderApple  at_;
    bool useFf_ = false;
};
#else
using AacDecoder = AacDecoderFfmpeg;
#endif

#endif
#if !defined(__ANDROID__) && !defined(__unix__) && !defined(__APPLE__)

/** ★★ NO PLATFORM DECODER HERE YET — Linux gets ffmpeg and macOS AudioToolbox, and until then
 *  available() answers honestly and the DAB+ services are marked unplayable rather than silently
 *  producing nothing. The ensemble, the MP2 services and the signal figures are unaffected. */
class AacDecoder {
public:
    static constexpr bool kExactFrames = true;
    bool available() const { return false; }
    const char* backend() const { return "none"; }
    bool decode(const uint8_t*, size_t, AacPcm&) { return false; }
    void drain(AacPcm&) {}
    void close() {}
    void reset() {}
    int  rateHz()   const { return 0; }
    int  channels() const { return 0; }
};

#endif

}  // namespace vibedab
