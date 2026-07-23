// Opus encode for the VibeServer audio path — the low-bandwidth codec for constrained links
// (the Apple-Watch-over-Bluetooth case; see BRIEF-vibeserver / memory vibeserver_link_management).
//
// ★ WHY Opus over the existing IMA-ADPCM: ~4x less bandwidth at BETTER quality (FM stereo is
// near-transparent at 64-96 kbps vs ADPCM's ~256 kbps), royalty-free, and its BITRATE is a smooth
// continuous lever the link ladder can ramp — where ADPCM only offered coarse stereo/rate steps.
//
// ★ GUARDED by VIBE_HAVE_OPUS. Only the macOS VibeServer core links libopus today (brew). The
// Android VibeServer keeps ADPCM until an NDK libopus-encode build lands; UberSDR *decode* on
// Android is ExoPlayer, not libopus, so there is nothing to reuse there. The Apple *decode* side
// already exists (watchOS OpusDecoder.swift + libopus.a) for when Jr gains a VibeServer backend.
#ifndef OPUS_AUDIO_ENCODER_H
#define OPUS_AUDIO_ENCODER_H

#include <cstdint>
#include <vector>

#ifdef VIBE_HAVE_OPUS
#include <opus/opus.h>

namespace vibe {

// Buffers interleaved int16 PCM into fixed Opus frames (20 ms @ 48 kHz = 960 samples/channel) and
// encodes each into a self-contained Opus packet. NOT thread-safe: drive it from the one audio
// thread that owns the stream, exactly like the ADPCM state.
class OpusAudioEncoder {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kFrameSamples = 960;   // 20 ms/channel — the standard Opus frame

    OpusAudioEncoder() = default;
    ~OpusAudioEncoder() { destroy(); }
    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    /// Set the target bitrate (bits/sec). Cheap to call every frame — this is the adaptive lever.
    /// Clamped to a sane range; takes effect immediately on a live encoder.
    void setBitrate(int bps) {
        if (bps < 6000) bps = 6000; else if (bps > 256000) bps = 256000;
        bitrate_ = bps;
        if (enc_) opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_));
    }
    int bitrate() const { return bitrate_; }

    /// Drop buffered samples + tear down the encoder — call on client change / rate change, so a
    /// new stream never starts mid-frame with stale state.
    void reset() { destroy(); buf_.clear(); }

    /// Feed one interleaved int16 block of `count` samples-per-channel across `channels` (1 or 2).
    /// Appends every completed Opus packet to `out` as {size prefix-free} — each vector entry is one
    /// packet. Returns via `out`; may append zero, one, or several packets depending on `count`.
    /// Reconfigures automatically if the channel count changes.
    void encode(const int16_t* pcm, int count, int channels,
                std::vector<std::vector<uint8_t>>& out) {
        if (count <= 0 || (channels != 1 && channels != 2)) return;
        if (channels != channels_) { destroy(); channels_ = channels; buf_.clear(); }
        if (!enc_ && !create()) return;

        buf_.insert(buf_.end(), pcm, pcm + (size_t)count * channels_);

        const size_t frameInterleaved = (size_t)kFrameSamples * channels_;
        size_t off = 0;
        uint8_t packet[4000];   // Opus tops out ~1275 B/frame; generous headroom
        while (buf_.size() - off >= frameInterleaved) {
            int n = opus_encode(enc_, buf_.data() + off, kFrameSamples, packet, sizeof packet);
            off += frameInterleaved;
            if (n > 0) out.emplace_back(packet, packet + n);
            // n <= 0 = encode error: skip this frame rather than wedging the stream.
        }
        if (off > 0) buf_.erase(buf_.begin(), buf_.begin() + off);
    }

private:
    bool create() {
        int err = 0;
        enc_ = opus_encoder_create(kSampleRate, channels_, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || !enc_) { enc_ = nullptr; return false; }
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_));
        // Music-leaning but voice-capable; low complexity keeps a Pi/phone server cheap.
        opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(6));
        return true;
    }
    void destroy() { if (enc_) { opus_encoder_destroy(enc_); enc_ = nullptr; } }

    OpusEncoder* enc_ = nullptr;
    int channels_ = 0;
    int bitrate_ = 64000;            // sensible FM-stereo default; the ladder overrides
    std::vector<int16_t> buf_;       // interleaved carry-over between callbacks
};

}  // namespace vibe
#endif  // VIBE_HAVE_OPUS
#endif  // OPUS_AUDIO_ENCODER_H
