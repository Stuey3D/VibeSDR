// vibe_dab_faad.h — DAB+ audio (HE-AAC v2: AAC-LC + SBR + PS, 960-sample frames) through FAAD2, IN PROCESS — the
// Linux and Mac servers' DAB+ decoder (vibe_dab_aacdec.h). V12, 2026-10-26; the only one there from 2026-10-29.
// ★★★ WHY FAAD2. It is told the truth about the frames — 960 samples (frameLengthFlag in the AudioSpecificConfig) — so
//     it runs DAB+'s real transform: exact pitch, an exact output rate, nothing to measure or re-pace. In process it is
//     also cheap, small and starts at once (no child process).
// ★★★ A LOST UNIT IS CONCEALED, NOT SPLICED. The service tells us exactly which access units never arrived (a CRC
//     failure leaves a hole in its super frame; a lost super frame a hole in the clock) and conceal() fills each with
//     one unit of audio: the last good unit DECODED AGAIN, fading out — the decoder's own overlap-add carries it on
//     without a step — then silence, and the next good unit fades back in.
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
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "vibedsp/simd_internal.h"
#include "vibe_lm_upsampler.h"   // ★ DabUpsampler (and its 2:3 kernel) — shared with the DRM xHE-AAC decoder
#include "vibe_thread.h"

extern "C" {
NeAACDecHandle vibedab_NeAACDecOpen(void);
NeAACDecConfigurationPtr vibedab_NeAACDecGetCurrentConfiguration(NeAACDecHandle);
unsigned char vibedab_NeAACDecSetConfiguration(NeAACDecHandle, NeAACDecConfigurationPtr);
char vibedab_NeAACDecInit2(NeAACDecHandle, unsigned char*, unsigned long, unsigned long*, unsigned char*);
void* vibedab_NeAACDecDecode(NeAACDecHandle, NeAACDecFrameInfo*, unsigned char*, unsigned long);
void vibedab_NeAACDecClose(NeAACDecHandle);
}

namespace vibedab {


/** ★ The face DabService drives (vibe_dab_aacdec.h), plus conceal(). PCM comes back
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

    /** One ADTS-framed access unit in (the very bytes the platform decoders get); 48 kHz stereo appended to out. */
    bool decode(const uint8_t* adts, size_t n, AacPcm& out) {
        if (n < 7) return true;
        const double c0 = cpuNow();
        const int profile = ((adts[2] >> 6) & 3) + 1, sfi = (adts[2] >> 2) & 15, ch = ((adts[2] & 1) << 2) | (adts[3] >> 6);
        if (!h_ || profile != prof_ || sfi != sfi_ || ch != ch_) {
            if (!open(profile, sfi, ch)) { cpu_ += cpuNow() - c0; return false; }
        }
        const size_t hdr = (adts[1] & 1) ? 7 : 9;
        if (n <= hdr) { cpu_ += cpuNow() - c0; return true; }
        std::vector<float>& pcm = runBuf_;
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
    /** ★ The split (test-dab-faad / dab-faad-bench, and the DAB panel's figure explained): FAAD2's own decode, and our
     *  rate conversion to 48 kHz. Both inside cpuSeconds(); the rest is the ADTS handling, concealment gain, copies. */
    double cpuDecodeSeconds() const { return cpuDecode_; }
    double cpuResampleSeconds() const { return cpuResample_; }
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
        const double d0 = cpuNow();
        void* pcm = vibedab_NeAACDecDecode(h_, &fi, const_cast<uint8_t*>(p), (unsigned long)n);
        cpuDecode_ += cpuNow() - d0;
        if (!pcm || fi.error || !fi.samples || !fi.channels) return false;
        const float* f = static_cast<const float*>(pcm);
        const size_t frames = fi.samples / fi.channels;
        st.resize(frames * 2);
        float peak = 0;
        static_assert(kScale == 1.0f, "the stereo fast path below copies FAAD2's samples unscaled");
        if (fi.channels == 2) {
            /* ★ the common case (stereo or PS): FAAD2's samples ARE the interleaved stereo — a copy, and the peak four
             *  samples at a time (NEON / SSE2 / scalar). The ARMv7 compiler leaves a float max loop scalar. */
            std::memcpy(st.data(), f, frames * 2 * sizeof(float));
            size_t i = 0;
#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
            float32x4_t m = vdupq_n_f32(0.0f);
            for (; i + 4 <= frames * 2; i += 4) m = vmaxq_f32(m, vabsq_f32(vld1q_f32(f + i)));
            const float32x2_t h = vpmax_f32(vget_low_f32(m), vget_high_f32(m));
            peak = std::max(vget_lane_f32(h, 0), vget_lane_f32(h, 1));
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)) && !defined(VIBE_FORCE_SCALAR)
            __m128 m = _mm_setzero_ps(); const __m128 sign = _mm_set1_ps(-0.0f);
            for (; i + 4 <= frames * 2; i += 4) m = _mm_max_ps(m, _mm_andnot_ps(sign, _mm_loadu_ps(f + i)));
            alignas(16) float mm[4]; _mm_store_ps(mm, m);
            peak = std::max(std::max(mm[0], mm[1]), std::max(mm[2], mm[3]));
#endif
            for (; i < frames * 2; ++i) peak = std::max(peak, std::fabs(f[i]));
        } else
        for (size_t i = 0; i < frames; ++i) {
            const float l = f[i * fi.channels], r = f[i * fi.channels + (fi.channels > 1 ? 1 : 0)];
            st[2 * i] = l * kScale; st[2 * i + 1] = r * kScale;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)) * kScale);
        }
        /* ★ The guard behind the CRC: real programme never comes out of a DAB+ decoder at +12 dBFS. A unit that does is
         *  damage that slipped through, and it is concealed rather than played. */
        if (peak > 4.0f) return false;
        rate_ = int(fi.samplerate); mono_ = fi.channels == 1;
        up_.configure(rate_, 48000);
        return true;
    }
    /** Resample to 48 kHz, apply the gain ramp (fade-in after a concealment), append. */
    void emit(const std::vector<float>& st, AacPcm& out, bool concealing) {
        std::vector<float>& o = emitBuf_; o.clear();
        const double r0 = cpuNow();
        up_.process(st.data(), st.size() / 2, o, mono_);
        cpuResample_ += cpuNow() - r0;
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
        std::vector<float>& st = runBuf_;
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

    /* ★ LEVEL: FAAD2's float output is already ±1.0 full scale (it divides by 2^15 itself). */
    static constexpr float kScale = 1.0f;
    NeAACDecHandle h_ = nullptr;
    int prof_ = -1, sfi_ = -1, ch_ = -1, rate_ = 0, unitFrames_ = 0, run_ = 0, mode_ = 0, reopened_ = 0, fadeMs_ = 120;
    float gain_ = 1.0f;
    bool mono_ = false;
    std::vector<uint8_t> last_;
    DabUpsampler up_;
    double cpu_ = 0, cpuDecode_ = 0, cpuResample_ = 0;
    std::vector<float> runBuf_, emitBuf_;   ///< reused: no allocation per unit on the decode thread
    int64_t decoded_ = 0, concealed_ = 0, faadErrors_ = 0;
};


/** ★★★ FAAD2 ON ITS OWN THREAD — vibe-faad2 (V12, 2026-10-26, Stuart's Pi 2).
 *  Decoded inline, FAAD2 (and its 32 → 48 kHz resampler) ran on the DAB worker that also carries the whole OFDM front
 *  end, FIC and Viterbi — already the busiest thread in the product on a Cortex-A7. One super frame's units arrive at
 *  once and are decoded at once, so every 120 ms the worker took a burst of codec work on top of its own; it missed its
 *  deadline and the audio broke up ("DAB breaking up like mad", with the machine only ~62 % busy over its four cores).
 *  This puts FAAD2 on a thread of its own, on another core.
 *  ★★ THE SAME FACE AS AacDecoderFaad, AND THE SAME ORDER OF EVENTS. DabService calls decode() / conceal() / reset()
 *     exactly as before; each becomes a JOB on a bounded single-producer single-consumer ring, executed in order on
 *     vibe-faad2, and the PCM goes to the service through the sink — the same pushPcm48Stereo it went through inline.
 *     Concealment is unchanged because it is the same AacDecoderFaad doing it, in the same sequence.
 *  ★★★ THE DAB THREAD NEVER WAITS. A full ring (or more than kMaxInFlightMs of programme already queued) means the codec
 *      cannot keep up: the unit is DROPPED, counted (dropped() — the health pill's "TOO HEAVY", vibehealth::BehindWatch),
 *      and carried as a missing unit on the next job that fits, where the decoder conceals it exactly as it conceals a
 *      CRC hole — so the programme clock stays whole and a drop is a fade, never a splice. No lock is taken on the
 *      producer side except an empty critical section to make the wake-up race-free (the consumer holds that mutex only
 *      while it checks the ring before sleeping).
 *  ★★ A RESET IS A GENERATION. reset() (service change, multiplex change, the owner's live switch) bumps gen_; jobs of an
 *     older generation are skipped unrun, the first job of the new one starts a fresh decoder, and the sink is told the
 *     generation of every block it receives — the service drops a block whose generation is stale, so audio of the old
 *     service can never land after the new one has begun.
 *  ★ Priority: vibeAudioDecodeThread — the audio class, one rung below the chain that feeds it (vibe_thread.h). */
class AacDecoderFaadAsync {
public:
    using Sink = std::function<void(uint32_t gen, const float* interleaved48, size_t nSamples)>;
    static constexpr bool kExactFrames = true;
    static constexpr int kSlots = 32;                 ///< ring capacity, in jobs (a job is one unit, or one run of concealment)
    static constexpr int kMaxInFlightMs = 500;        ///< programme queued beyond this is a codec that cannot keep up
    bool exactFrames() const { return true; }
    bool canConceal() const { return true; }
    bool async() const { return true; }
    const char* backend() const { return "FAAD2"; }
    bool available() const { return true; }
    AacDecoderFaadAsync() { for (auto& j : ring_) j.bytes.reserve(4096); }
    ~AacDecoderFaadAsync() { stop(); }
    AacDecoderFaadAsync(const AacDecoderFaadAsync&) = delete;
    AacDecoderFaadAsync& operator=(const AacDecoderFaadAsync&) = delete;

    /** The service's PCM entry point. Set once, before the first unit; called on vibe-faad2. */
    void setSink(Sink s) { sink_ = std::move(s); }
    /** ★ Test knob: hold the worker (it takes no job while paused) — how test-dab-faad-queue fills the ring. */
    void setPausedForTest(bool p) { paused_.store(p); if (!p) wake_(); }

    /** One ADTS unit — queued, never decoded here. Always "true": a unit the codec rejects is concealed over there. */
    bool decode(const uint8_t* adts, size_t n, AacPcm&) { submit_(kDecode, adts, n, 1); return true; }
    /** `units` missing units, queued in order with the decodes around them. */
    void conceal(int units, AacPcm&) { if (units > 0) submit_(kConceal, nullptr, 0, units); }
    void drain(AacPcm&) {}
    /** ★ A new stream: a new generation. Nothing waits — the worker drops the old generation's jobs as it reaches them. */
    void reset() {
        gen_.fetch_add(1, std::memory_order_acq_rel);
        owed_ = 0; unitFrames_.store(0, std::memory_order_relaxed); open_.store(false, std::memory_order_relaxed);
        wake_();
    }
    void close() { reset(); }
    /** Stops and joins the worker (the service's own stop, and the destructor). The next unit starts it again. */
    void stop() {
        { std::lock_guard<std::mutex> lk(wm_); stop_ = true; }
        wcv_.notify_all();
        if (th_.joinable()) th_.join();
        stop_ = false;
    }

    uint32_t generation() const { return gen_.load(std::memory_order_acquire); }
    int unitFrames() const { return unitFrames_.load(std::memory_order_relaxed); }
    /** ★★ 48 kHz frames owed by the codec but not yet pushed: the queue plus the units carried after a drop. DabService's
     *  holdTimeline adds this to what it has been pushed, or it would fill with silence audio that is merely in flight. */
    uint64_t inFlightFrames() const {
        const int uf = unitFrames(); const uint64_t per = uf > 0 ? uint64_t(uf) : 2880u;   // unknown yet: a 60 ms unit
        return (uint64_t(queuedUnits_.load(std::memory_order_acquire)) + uint64_t(owed_)) * per;
    }
    int rateHz() const { return open_.load(std::memory_order_relaxed) ? 48000 : 0; }
    int channels() const { return open_.load(std::memory_order_relaxed) ? 2 : 0; }
    double cpuSeconds() const { return cpu_.load(std::memory_order_relaxed); }
    double cpuDecodeSeconds() const { return cpuDec_.load(std::memory_order_relaxed); }
    double cpuResampleSeconds() const { return cpuRes_.load(std::memory_order_relaxed); }
    int64_t concealed() const { return concealed_.load(std::memory_order_relaxed); }
    int64_t decoded() const { return decoded_.load(std::memory_order_relaxed); }
    /** Units the codec could not take in time — monotonic for the life of the decoder (BehindWatch reads it). */
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    int restarts() const { return restarts_.load(std::memory_order_relaxed); }
    int queuedJobs() const { return int(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)); }
    void setFadeMs(int ms) { fadeMs_.store(ms); }

private:
    enum Kind : uint8_t { kDecode = 0, kConceal = 1 };
    struct Job { uint32_t gen = 0; Kind kind = kDecode; int units = 0; int concealBefore = 0; std::vector<uint8_t> bytes; };

    /** Producer side (the DAB thread) — never blocks. */
    void submit_(Kind k, const uint8_t* p, size_t n, int units) {
        if (!th_.joinable()) th_ = std::thread([this] { loop_(); });
        const uint64_t h = head_.load(std::memory_order_relaxed), t = tail_.load(std::memory_order_acquire);
        const int uf = unitFrames(); const uint64_t per = uf > 0 ? uint64_t(uf) : 2880u;
        // ★ what is QUEUED, not what is owed: units already dropped must never keep the next one out (or one bad moment
        //   would lock the ring shut for good); they ride on the next job that fits and are concealed in a few ms
        const uint64_t queuedMs = uint64_t(queuedUnits_.load(std::memory_order_acquire)) * per / 48;
        if (h - t >= uint64_t(kSlots) || queuedMs > uint64_t(kMaxInFlightMs)) {
            // ★★ behind: this unit becomes a missing one, concealed in its place when the next job fits
            owed_ += units;
            if (k == kDecode) dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Job& j = ring_[h % kSlots];
        j.gen = gen_.load(std::memory_order_acquire); j.kind = k; j.units = units; j.concealBefore = owed_;
        if (p && n) j.bytes.assign(p, p + n); else j.bytes.clear();
        queuedUnits_.fetch_add(units + owed_, std::memory_order_acq_rel);
        owed_ = 0;
        head_.store(h + 1, std::memory_order_release);
        wake_();
    }
    void wake_() { { std::lock_guard<std::mutex> lk(wm_); } wcv_.notify_one(); }

    /** Consumer side — vibe-faad2. */
    void loop_() {
        vibeAudioDecodeThread("vibe-faad2");   // ★ ≤ 10 characters: top -H shows it whole (vibe-dabclk reads "vibe-dabc+")
        AacPcm out; out.interleaved.reserve(48000);
        uint32_t applied = gen_.load() - 1;   // ★ forces a fresh decoder on the first job
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(wm_);
                wcv_.wait(lk, [&] { return stop_ || (!paused_.load() && tail_.load(std::memory_order_relaxed) != head_.load(std::memory_order_acquire)); });
                if (stop_) return;
            }
            const uint64_t t = tail_.load(std::memory_order_relaxed);
            Job& j = ring_[t % kSlots];
            const uint32_t cur = gen_.load(std::memory_order_acquire);
            const int units = j.units + j.concealBefore;
            if (j.gen == cur) {
                if (j.gen != applied) { dec_.reset(); applied = j.gen; }
                if (const int f = fadeMs_.load(); f > 0) dec_.setFadeMs(f);
                out.interleaved.clear();
                if (j.concealBefore > 0) dec_.conceal(j.concealBefore, out);
                if (j.kind == kDecode) dec_.decode(j.bytes.data(), j.bytes.size(), out);
                else dec_.conceal(j.units, out);
                if (dec_.unitFrames() > 0) { unitFrames_.store(dec_.unitFrames(), std::memory_order_relaxed); open_.store(true, std::memory_order_relaxed); }
                cpu_.store(dec_.cpuSeconds(), std::memory_order_relaxed);
                cpuDec_.store(dec_.cpuDecodeSeconds(), std::memory_order_relaxed);
                cpuRes_.store(dec_.cpuResampleSeconds(), std::memory_order_relaxed);
                concealed_.store(dec_.concealed(), std::memory_order_relaxed);
                decoded_.store(dec_.decoded(), std::memory_order_relaxed);
                restarts_.store(dec_.restarts(), std::memory_order_relaxed);
                if (!out.interleaved.empty() && sink_) sink_(j.gen, out.interleaved.data(), out.interleaved.size());
            }
            // ★ AFTER the push: pushed + in-flight never dips below the truth while a block crosses over
            queuedUnits_.fetch_sub(units, std::memory_order_acq_rel);
            tail_.store(t + 1, std::memory_order_release);
        }
    }

    AacDecoderFaad dec_;                       ///< touched ONLY by vibe-faad2 (and its own reset, there)
    Sink sink_;
    Job ring_[kSlots];
    std::atomic<uint64_t> head_{0}, tail_{0};
    std::atomic<uint32_t> gen_{0};
    std::atomic<int> queuedUnits_{0};
    int owed_ = 0;                             ///< producer only: units dropped, carried to the next job that fits
    std::atomic<int> unitFrames_{0}, restarts_{0}, fadeMs_{0};
    std::atomic<bool> open_{false}, paused_{false};
    std::atomic<double> cpu_{0}, cpuDec_{0}, cpuRes_{0};
    std::atomic<int64_t> concealed_{0}, decoded_{0};
    std::atomic<uint64_t> dropped_{0};
    std::mutex wm_;
    std::condition_variable wcv_;
    bool stop_ = false;
    std::thread th_;
};

}  // namespace vibedab
#endif
