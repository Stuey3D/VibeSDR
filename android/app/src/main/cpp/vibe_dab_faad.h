// vibe_dab_faad.h — DAB+ audio (HE-AAC v2: AAC-LC + SBR + PS, 960-sample frames) through FAAD2, IN PROCESS — the
// alternative to the FFmpeg child process in vibe_dab_aacdec.h. V12, 2026-10-26.
//
// ★★★ NOT THE DEFAULT. Stuart: "our DAB+ audio gets high praise" — FFmpeg stays the default until he has listened to
//     both (docs/v12/DRM-AUDIO-ROUTES.md "DAB+"); the owner's switch (dabFaad2) or VIBE_DAB_AAC=faad2 picks this one.
// ★★★ WHY IT IS WORTH HAVING. FAAD2 is told the truth about the frames — 960 samples (frameLengthFlag in the
//     AudioSpecificConfig) — so it runs DAB+'s real transform: exact pitch, an exact output rate, nothing to measure or
//     re-pace. FFmpeg is fed ADTS, which cannot say 960, decodes each unit as 1024 and the service paces it at the
//     MEASURED rate (+0.2 % on tones). In process it is also ~5x cheaper, ~80x smaller and starts at once (no child).
// ★★★ A LOST UNIT IS CONCEALED, NOT SPLICED. The service tells us exactly which access units never arrived (a CRC
//     failure leaves a hole in its super frame; a lost super frame a hole in the clock) and conceal() fills each with
//     one unit of audio: the last good unit DECODED AGAIN, fading out — the decoder's own overlap-add carries it on
//     without a step — then silence, and the next good unit fades back in. The FFmpeg path cannot do that: a child
//     process given only the good units joins their neighbours end to end.
// ★★ THE PLAIN FAAD2 BUILD, WITH ITS SYMBOLS PREFIXED vibedab_. The server also links FAAD2's DRM build for DRM, and
//    that build refuses DAB+ ("No standard extension payload allowed in DRM"); both export the same names, so the deps
//    builders compile the plain one a second time with every global renamed (libfaad_dab.a, faad_dab_prefix.h). This
//    file declares the six entry points it uses under their prefixed names — never through the macro header, which
//    would rename the DRM decoder's calls in the same translation unit too.
// ★ GPL-2.0-or-later, server builds only (VIBE_HAVE_FAAD2_DAB) — never the App Store app (build_ios.sh checks).
// ★ Include vibe_dab_aacdec.h, not this file: it brings this one in where AacPcm is defined and wraps it in the switch.
#pragma once
#if defined(VIBE_HAVE_FAAD2_DAB)
#include <neaacdec.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <vector>
#include "vibedsp/simd_internal.h"

extern "C" {
NeAACDecHandle vibedab_NeAACDecOpen(void);
NeAACDecConfigurationPtr vibedab_NeAACDecGetCurrentConfiguration(NeAACDecHandle);
unsigned char vibedab_NeAACDecSetConfiguration(NeAACDecHandle, NeAACDecConfigurationPtr);
char vibedab_NeAACDecInit2(NeAACDecHandle, unsigned char*, unsigned long, unsigned long*, unsigned char*);
void* vibedab_NeAACDecDecode(NeAACDecHandle, NeAACDecFrameInfo*, unsigned char*, unsigned long);
void vibedab_NeAACDecClose(NeAACDecHandle);
}

namespace vibedab {

/** ★ 32 kHz → 48 kHz (and any other small L/M), stereo, polyphase windowed-sinc. HE-AAC on a 16 kHz core decodes to
 *  32 kHz and its SBR band reaches ~15 kHz, so the filter must pass that — a generic audio resampler cut at 0.9 × ¼ of
 *  the up-sampled rate would dull the top octave. Kaiser β 8.6 (≈ −85 dB), 64 taps a branch: passband to ~14.5 kHz,
 *  images of the top 1 kHz below the input's Nyquist land above 16 kHz at < −80 dB by 17 kHz. 64 MACs per output per
 *  channel through vibedsp's NEON/SSE2 dot product: ~6 M MAC/s for a 32 kHz service — under 1 % of a Pi 2 core. */
class DabUpsampler {
public:
    void configure(int inRate, int outRate) {
        if (inRate == in_ && outRate == out_) return;
        in_ = inRate; out_ = outRate;
        int a = inRate, b = outRate; while (b) { const int t = a % b; a = b; b = t; }
        L_ = outRate / a; M_ = inRate / a;
        const int N = L_ * kP;
        const double fc = 0.5 * std::min(inRate, outRate) * 0.93 / (double(inRate) * L_);   // cycles per up-sample
        std::vector<double> h(static_cast<size_t>(N));
        double sum = 0;
        for (int n = 0; n < N; ++n) {
            const double x = n - (N - 1) / 2.0;
            const double s = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            const double r = 2.0 * n / (N - 1) - 1.0;
            h[size_t(n)] = s * bessel0(8.6 * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(8.6);
            sum += h[size_t(n)];
        }
        rb_.assign(size_t(N), 0.0f);
        for (int b2 = 0; b2 < L_; ++b2)
            for (int m = 0; m < kP; ++m) rb_[size_t(b2) * kP + size_t(m)] = float(h[size_t(b2 + (kP - 1 - m) * L_)] * L_ / sum);
        reset();
    }
    void reset() { for (auto& c : hist_) c.assign(kP - 1, 0.0f); ph_ = 0; }
    /** Interleaved stereo in (n frames) → interleaved stereo appended to out. Passes straight through when in == out. */
    void process(const float* in, size_t n, std::vector<float>& out) {
        if (L_ == M_) { out.insert(out.end(), in, in + 2 * n); return; }
        for (int c = 0; c < 2; ++c) { auto& h = hist_[c]; const size_t k = h.size(); h.resize(k + n); for (size_t i = 0; i < n; ++i) h[k + i] = in[2 * i + size_t(c)]; }
        const size_t have = hist_[0].size();               // window = [kP-1 history][n new]
        // output j uses input index base = (ph_ / L) + kP-1 (relative to hist start) and branch ph_ % L
        while (true) {
            const long long base = ph_ / L_;
            if (size_t(base) + kP > have) break;
            const float* t = &rb_[size_t(ph_ % L_) * kP];
            out.push_back(vibedsp::dotReal(t, &hist_[0][size_t(base)], kP));
            out.push_back(vibedsp::dotReal(t, &hist_[1][size_t(base)], kP));
            ph_ += M_;
        }
        const long long used = ph_ / L_;                   // inputs no output will need again
        for (auto& h : hist_) h.erase(h.begin(), h.begin() + long(used));
        ph_ -= used * L_;
    }
    int outRate() const { return out_; }
private:
    static constexpr int kP = 64;
    static double bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }
    int in_ = 0, out_ = 0, L_ = 1, M_ = 1;
    long long ph_ = 0;
    std::vector<float> rb_, hist_[2];
};

/** ★ The same face as AacDecoderFfmpeg (vibe_dab_aacdec.h) — DabService drives either — plus conceal(). PCM comes back
 *  as 48 kHz interleaved stereo, ready for the service's fast path (no interpolation there). */
class AacDecoderFaad {
public:
    static constexpr bool kExactFrames = true;
    bool exactFrames() const { return true; }
    bool canConceal() const { return true; }
    const char* backend() const { return "FAAD2"; }
    bool available() const { return true; }
    int restarts() const { return reopened_; }
    AacDecoderFaad() = default;
    ~AacDecoderFaad() { close(); }
    AacDecoderFaad(const AacDecoderFaad&) = delete;
    AacDecoderFaad& operator=(const AacDecoderFaad&) = delete;

    /** One ADTS-framed access unit in (the very bytes the FFmpeg path gets); 48 kHz stereo appended to out. */
    bool decode(const uint8_t* adts, size_t n, AacPcm& out) {
        if (n < 7) return true;
        const double c0 = cpuNow();
        const int profile = ((adts[2] >> 6) & 3) + 1, sfi = (adts[2] >> 2) & 15, ch = ((adts[2] & 1) << 2) | (adts[3] >> 6);
        if (!h_ || profile != prof_ || sfi != sfi_ || ch != ch_) {
            if (!open(profile, sfi, ch)) { cpu_ += cpuNow() - c0; return false; }
        }
        const size_t hdr = (adts[1] & 1) ? 7 : 9;
        if (n <= hdr) { cpu_ += cpuNow() - c0; return true; }
        std::vector<float> pcm;
        const bool ok = run(adts + hdr, n - hdr, pcm);
        if (!ok) {
            /* ★ A unit that passed DAB+'s CRC and still would not decode (a CRC false pass, 1 in 65536 of the bad ones,
             *  or a bitstream FAAD2 rejects): concealed like a missing one — never played. */
            ++faadErrors_;
            concealOne(out);
        } else {
            last_.assign(adts + hdr, adts + n);
            emit(pcm, out, false);
            ++decoded_;
        }
        out.rateHz = 48000; out.channels = 2;
        cpu_ += cpuNow() - c0;
        return true;
    }
    /** ★★ `units` access units that never arrived — each becomes exactly one unit of audio (the clock stays whole). */
    void conceal(int units, AacPcm& out) {
        const double c0 = cpuNow();
        for (int i = 0; i < units; ++i) concealOne(out);
        out.rateHz = 48000; out.channels = 2;
        cpu_ += cpuNow() - c0;
    }
    /** 48 kHz frames one access unit fills — 0 until the first unit has been decoded. */
    int unitFrames() const { return unitFrames_; }
    void drain(AacPcm&) {}
    void close() { if (h_) { vibedab_NeAACDecClose(h_); h_ = nullptr; } prof_ = sfi_ = ch_ = -1; }
    void reset() { close(); last_.clear(); gain_ = 1.0f; run_ = 0; unitFrames_ = 0; up_.reset(); }
    int rateHz() const { return h_ ? 48000 : 0; }
    int channels() const { return h_ ? 2 : 0; }
    double cpuSeconds() const { return cpu_; }
    int64_t decoded() const { return decoded_; }
    int64_t concealed() const { return concealed_; }
    int64_t faadErrors() const { return faadErrors_; }
    /** ★ Test knob: the concealment's shape. 0 = repeat-and-fade (the default), 1 = silence at once (what a plain
     *  "drop the unit" decoder would sound like, if it kept the clock). */
    void setConcealMode(int m) { mode_ = m; }

private:
    static double cpuNow() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return double(t.tv_sec) + double(t.tv_nsec) * 1e-9; }
    bool open(int profile, int sfi, int ch) {
        close();
        h_ = vibedab_NeAACDecOpen();
        if (!h_) return false;
        NeAACDecConfigurationPtr c = vibedab_NeAACDecGetCurrentConfiguration(h_);
        c->outputFormat = FAAD_FMT_FLOAT; c->downMatrix = 0;
        vibedab_NeAACDecSetConfiguration(h_, c);
        // AudioSpecificConfig: object type, sampling index, channels, GASpecificConfig with frameLengthFlag = 1 (960)
        const uint16_t asc = uint16_t((profile << 11) | (sfi << 7) | (ch << 3) | (1 << 2));
        unsigned char a[2] = { uint8_t(asc >> 8), uint8_t(asc) };
        unsigned long sr = 0; unsigned char chs = 0;
        if (vibedab_NeAACDecInit2(h_, a, 2, &sr, &chs) < 0) { close(); return false; }
        if (prof_ != -1 || decoded_) ++reopened_;
        prof_ = profile; sfi_ = sfi; ch_ = ch; last_.clear(); gain_ = 1.0f; run_ = 0; up_.reset();
        return true;
    }
    /** Decode one raw unit into interleaved stereo at FAAD2's own rate; false on any error or a guard trip. */
    bool run(const uint8_t* p, size_t n, std::vector<float>& st) {
        NeAACDecFrameInfo fi{};
        void* pcm = vibedab_NeAACDecDecode(h_, &fi, const_cast<uint8_t*>(p), (unsigned long)n);
        if (!pcm || fi.error || !fi.samples || !fi.channels) return false;
        const float* f = static_cast<const float*>(pcm);
        const size_t frames = fi.samples / fi.channels;
        st.resize(frames * 2);
        float peak = 0;
        for (size_t i = 0; i < frames; ++i) {
            const float l = f[i * fi.channels], r = f[i * fi.channels + (fi.channels > 1 ? 1 : 0)];
            st[2 * i] = l * kScale; st[2 * i + 1] = r * kScale;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)) * kScale);
        }
        /* ★ The guard behind the CRC: real programme never comes out of a DAB+ decoder at +12 dBFS. A unit that does is
         *  damage that slipped through, and it is concealed rather than played. */
        if (peak > 4.0f) return false;
        rate_ = int(fi.samplerate);
        up_.configure(rate_, 48000);
        return true;
    }
    /** Resample to 48 kHz, apply the gain ramp (fade-in after a concealment), append. */
    void emit(const std::vector<float>& st, AacPcm& out, bool concealing) {
        std::vector<float> o; o.reserve(st.size() * 2);
        up_.process(st.data(), st.size() / 2, o);
        const size_t frames = o.size() / 2;
        if (!concealing) {
            unitFrames_ = int(frames);
            if (gain_ < 1.0f) {                               // ★ coming back: raised-cosine up from wherever the fade left it
                const float g0 = gain_;
                for (size_t i = 0; i < frames; ++i) {
                    const float g = g0 + (1.0f - g0) * 0.5f * (1.0f - std::cos(float(M_PI) * float(i + 1) / float(frames)));
                    o[2 * i] *= g; o[2 * i + 1] *= g;
                }
                gain_ = 1.0f;
            }
            run_ = 0;
        }
        out.interleaved.insert(out.interleaved.end(), o.begin(), o.end());
    }
    /** ★★ ONE missing unit. The first: the last good unit decoded again, fading to nothing over its length (raised
     *  cosine) — the decoder's overlap-add joins it to what played before without a step. Any after it: silence, the
     *  decoder untouched. The next good unit fades back in (emit). Before any unit has decoded there is nothing to
     *  conceal with and nothing to keep in time: the caller has no clock to hold yet either. */
    void concealOne(AacPcm& out) {
        if (!unitFrames_) return;
        ++concealed_; ++run_;
        std::vector<float> st;
        if (mode_ == 0 && run_ <= fadeUnits() && !last_.empty() && h_ && run(last_.data(), last_.size(), st)) {
            const size_t base = out.interleaved.size();
            emit(st, out, true);
            const size_t frames = (out.interleaved.size() - base) / 2;
            // ★ one raised-cosine fade across fadeUnits_ units: this unit covers [run_-1, run_] of it, from the gain
            //   the last one left (a fade-in cut short by a new hole starts lower, never jumps up)
            const float g0 = gain_, g1 = 0.5f * (1.0f + std::cos(float(M_PI) * float(run_) / float(fadeUnits())));
            const float gEnd = std::min(g0, g1);
            for (size_t i = 0; i < frames; ++i) {
                const float u = 0.5f * (1.0f - std::cos(float(M_PI) * float(i + 1) / float(frames)));
                const float g = g0 + (gEnd - g0) * u;
                out.interleaved[base + 2 * i] *= g; out.interleaved[base + 2 * i + 1] *= g;
            }
            gain_ = gEnd;
            return;
        }
        // ★ Past the fade: silence, at the gain the fade reached (0) — exact, not a step: the fade ended at 0.
        if (gain_ > 0.0f && !out.interleaved.empty()) {   // a fade that could not run (no unit to repeat): short ramp
            const size_t m = std::min(out.interleaved.size() / 2, size_t(96));
            const size_t base = out.interleaved.size() - 2 * m;
            for (size_t i = 0; i < m; ++i) { const float g = float(m - 1 - i) / float(m); out.interleaved[base + 2 * i] *= g; out.interleaved[base + 2 * i + 1] *= g; }
        }
        out.interleaved.insert(out.interleaved.end(), size_t(unitFrames_) * 2, 0.0f);
        gain_ = 0.0f;
    }
public:
    /** ★ How long the repeat-and-fade lasts before silence: kFadeMs of programme, in whole units (60 ms units → 2, 20 ms
     *  units → 6). Long enough that a one-unit hole is a −6 dB dip rather than a drop-out, short enough that a longer
     *  hole never sounds like a stuck record. setFadeMs is the test knob. */
    void setFadeMs(int ms) { fadeMs_ = std::max(1, ms); }
    int fadeUnits() const { return unitFrames_ ? std::max(1, int(std::lround(double(fadeMs_) * 48.0 / double(unitFrames_)))) : 1; }
private:

    /* ★ LEVEL: FAAD2's float output is already ±1.0 full scale (it divides by 2^15 itself), FFmpeg's f32le the same.
     *  Any difference between the two is measured in test-dab-faad, not assumed here. */
    static constexpr float kScale = 1.0f;
    NeAACDecHandle h_ = nullptr;
    int prof_ = -1, sfi_ = -1, ch_ = -1, rate_ = 0, unitFrames_ = 0, run_ = 0, mode_ = 0, reopened_ = 0, fadeMs_ = 120;
    float gain_ = 1.0f;
    std::vector<uint8_t> last_;
    DabUpsampler up_;
    double cpu_ = 0;
    int64_t decoded_ = 0, concealed_ = 0, faadErrors_ = 0;
};

}  // namespace vibedab
#endif
