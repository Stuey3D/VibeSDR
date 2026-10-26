// vibe_dab_faad.h — DAB+ audio (HE-AAC v2: AAC-LC + SBR + PS, 960-sample frames) through FAAD2, IN PROCESS — the
// candidate replacement for the FFmpeg child process in vibe_dab_aacdec.h. V12, 2026-10-26.
//
// ★★★ NOT THE DEFAULT. Stuart: "our DAB+ audio gets high praise" — the switch is gated on proving no regression on every
//     measure (test-dab-faad, docs/v12/DRM-AUDIO-ROUTES.md "DAB+"). Until he decides, this is used only by that test.
// ★★ What it does differently, by construction: FAAD2 is told the frames are 960 samples (frameLengthFlag in the
//    AudioSpecificConfig), so it runs DAB+'s real transform. The FFmpeg path is fed ADTS, which cannot say 960, so FFmpeg
//    decodes each unit as 1024 and the service paces the result at the MEASURED rate (vibe_dab_service.h). The two
//    therefore cannot match sample for sample — the test measures how far apart they are and which is right.
// ★ GPL-2.0-or-later, server builds only (VIBE_HAVE_FAAD2) — never the App Store app (build_ios.sh checks).
#pragma once
#if defined(VIBE_HAVE_FAAD2)
#include <neaacdec.h>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibedab {

class DabFaad {
public:
    ~DabFaad() { close(); }
    /** One ADTS-framed access unit in (as the FFmpeg path is fed); interleaved stereo float out at rateHz(). */
    bool decode(const uint8_t* adts, size_t n, std::vector<float>& out) {
        if (n < 7) return false;
        const int profile = ((adts[2] >> 6) & 3) + 1, sfi = (adts[2] >> 2) & 15, ch = ((adts[2] & 1) << 2) | (adts[3] >> 6);
        if (!h_ || profile != prof_ || sfi != sfi_ || ch != ch_) {
            close();
            h_ = NeAACDecOpen();
            NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(h_);
            c->outputFormat = FAAD_FMT_FLOAT; c->downMatrix = 0;
            NeAACDecSetConfiguration(h_, c);
            // AudioSpecificConfig: object type, sampling index, channels, GASpecificConfig with frameLengthFlag = 1 (960)
            const uint16_t asc = uint16_t((profile << 11) | (sfi << 7) | (ch << 3) | (1 << 2));
            unsigned char a[2] = { uint8_t(asc >> 8), uint8_t(asc) };
            unsigned long sr = 0; unsigned char chs = 0;
            if (NeAACDecInit2(h_, a, 2, &sr, &chs) < 0) { close(); return false; }
            prof_ = profile; sfi_ = sfi; ch_ = ch; last_.clear();
        }
        const size_t hdr = (adts[1] & 1) ? 7 : 9;
        if (n <= hdr) return true;
        NeAACDecFrameInfo fi{};
        void* pcm = NeAACDecDecode(h_, &fi, const_cast<uint8_t*>(adts + hdr), (unsigned long)(n - hdr));
        if (!pcm || fi.error || !fi.samples || !fi.channels) {
            ++bad_;
            if (!last_.empty()) {   // ★ conceal: the last good unit again, fading out; the next good one fades in
                const size_t m = last_.size() / 2;
                for (size_t i = 0; i < m; ++i) { const float g = float(m - i) / float(m) * gain_; out.push_back(last_[2 * i] * g); out.push_back(last_[2 * i + 1] * g); }
                gain_ = 0.0f; fadeIn_ = true;
            }
            return true;
        }
        ++good_;
        rate_ = int(fi.samplerate);
        const float* f = static_cast<const float*>(pcm);
        const size_t frames = fi.samples / fi.channels;
        last_.resize(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            last_[2 * i] = f[i * fi.channels];
            last_[2 * i + 1] = f[i * fi.channels + (fi.channels > 1 ? 1 : 0)];
        }
        const size_t base = out.size();
        out.insert(out.end(), last_.begin(), last_.end());
        if (fadeIn_) {
            const size_t m = std::min<size_t>(frames, size_t(rate_ / 100));
            for (size_t i = 0; i < m; ++i) { const float g = float(i) / float(m); out[base + 2 * i] *= g; out[base + 2 * i + 1] *= g; }
            fadeIn_ = false;
        }
        gain_ = 1.0f;
        return true;
    }
    int rateHz() const { return rate_; }
    int64_t good() const { return good_; }
    int64_t bad() const { return bad_; }
    void close() { if (h_) { NeAACDecClose(h_); h_ = nullptr; } prof_ = sfi_ = ch_ = -1; }
private:
    NeAACDecHandle h_ = nullptr;
    int prof_ = -1, sfi_ = -1, ch_ = -1, rate_ = 0;
    std::vector<float> last_; float gain_ = 1.0f; bool fadeIn_ = false;
    int64_t good_ = 0, bad_ = 0;
};

}  // namespace vibedab
#endif
