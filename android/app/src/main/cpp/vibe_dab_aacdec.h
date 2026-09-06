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

#if defined(__ANDROID__)
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
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
        return true;
    }

    AMediaCodec* codec_ = nullptr;
    int      rate_ = 0, ch_ = 0;
    int64_t  pts_  = 0;
    bool     failed_ = false;
};

#else

/** ★★ NO PLATFORM DECODER HERE YET — Linux gets ffmpeg and macOS AudioToolbox, and until then
 *  available() answers honestly and the DAB+ services are marked unplayable rather than silently
 *  producing nothing. The ensemble, the MP2 services and the signal figures are unaffected. */
class AacDecoder {
public:
    bool available() const { return false; }
    bool decode(const uint8_t*, size_t, AacPcm&) { return false; }
    void drain(AacPcm&) {}
    void close() {}
    void reset() {}
    int  rateHz()   const { return 0; }
    int  channels() const { return 0; }
};

#endif

}  // namespace vibedab
