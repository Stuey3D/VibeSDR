// vibe_m17.h — M17 DIGITAL VOICE AND DATA AS A MODE (V12, docs/v12/M17.md). M17 is an open amateur protocol (the M17
// Protocol Specification, spec.m17project.org): 4-FSK at 4800 symbols a second in a 9 kHz channel on VHF/UHF, frames of
// 40 ms, Codec 2 voice. It is chosen like NFM — a listener's demodulator — and the decoded speech replaces the FM audio.
//
// The pieces, in the order the signal meets them:
//   1. Demod: the listener's channel IQ → 24 kHz → a channel filter → the FM discriminator (scaled so ±800 Hz is ±1) → the
//      root-raised-cosine matched filter (α = 0.5) → sync correlation against every frame type's burst, which gives the
//      frame AND its timing (the peak, interpolated to a fraction of a sample); level and DC (the tuning error) are fitted
//      from the known sync symbols and then from all 192 decided symbols; soft symbols out. While a transmission is
//      followed, the next sync is looked for 40 ms on, a few samples either side, and a frame whose sync is lost in a fade
//      is still taken at its expected place ("flywheel") for a few frames.
//   2. Receiver: soft bits (the randomiser and the QPP interleaver undone), the Viterbi, Golay for the LICH, the LSF and its
//      CRC, the LSF rebuilt from the LICH for a late joiner (fragments kept across superframes and combined until the CRC
//      passes), the frame number, the voice (Codec 2 3200, or 1600 with data), META (text, GNSS, the extended callsign),
//      packets (SMS shown as text, the rest counted) — and the report.
//   3. Listener: the thread, the bounded queues and the 48 kHz speech buffer (FreeDV's pattern, vibe_freedv.h).
//
// ★★★ NOT A FALSE STATION ON NOISE. A sync burst is only 8 symbols, and band noise through an FM discriminator matches one
//     dozens of times a second. So nothing is believed on correlation alone: an LSF must pass its CRC; a stream frame
//     joined late must decode its LICH (four Golay words) with few channel errors, and is not played until the NEXT frame
//     arrives 40 ms later with the next frame number; a packet must pass its CRC. The noise test runs minutes of noise and
//     requires zero frames.
// ★★ THE VOCODER IS THE SYSTEM'S libcodec2, through FreeDV's loader (vibe_freedv.h — one dlopen for both modes). The mode
//    is offered only where it loaded and can run Codec 2 3200 ("no dead controls").
// ★★ THREADING (Network > Audio > Spectrum > Decoders): the pipeline thread only COPIES its IQ into a bounded queue (the
//    oldest dropped and counted past 2 s); everything above runs on a vibe-m17 thread at the audio-codec rung (as FreeDV's
//    modem does); the audio callback takes exactly the frames it sends from a speech buffer — silence until primed, silence
//    on underrun, the oldest dropped past 1.5 s.
#pragma once
#include "vibe_m17_fec.h"
#include "vibe_freedv.h"   // ★ the shared libcodec2 loader (vibe::freedv::vocoder)
#include "vibedsp/vibedsp.h"
#include "vibe_thread.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vibe {
namespace m17 {
using cf32 = std::complex<float>;

// ── Filters ───────────────────────────────────────────────────────────────────────────────────────────────────────────
/** Root-raised-cosine taps, `sps` samples a symbol, `span` symbols, normalised so the taps sum to `dcGain`. */
inline std::vector<float> rrcTaps(int sps, double alpha, int span, double dcGain) {
    const int n = sps * span + 1;
    std::vector<float> h(static_cast<size_t>(n));
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double t = double(i - n / 2) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1.0 - alpha + 4.0 * alpha / M_PI;
        else if (std::fabs(std::fabs(t) - 1.0 / (4.0 * alpha)) < 1e-9)
            v = alpha / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * alpha)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * alpha)));
        else v = (std::sin(M_PI * t * (1 - alpha)) + 4 * alpha * t * std::cos(M_PI * t * (1 + alpha)))
                 / (M_PI * t * (1 - (4 * alpha * t) * (4 * alpha * t)));
        h[size_t(i)] = float(v); sum += v;
    }
    for (auto& v : h) v = float(v * dcGain / sum);
    return h;
}
/** A Blackman-windowed sinc low-pass, unit DC gain. */
inline std::vector<float> lowpassTaps(double fs, double cutHz, int n) {
    std::vector<float> h(static_cast<size_t>(n));
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double m = i - (n - 1) / 2.0;
        const double x = 2 * cutHz / fs;
        const double s = std::fabs(m) < 1e-9 ? x : std::sin(M_PI * x * m) / (M_PI * m);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (n - 1)) + 0.08 * std::cos(4 * M_PI * i / (n - 1));
        h[size_t(i)] = float(s * w); sum += s * w;
    }
    for (auto& v : h) v = float(v / sum);
    return h;
}

// ── The transmitter (the tests' and the benchmark row's) ──────────────────────────────────────────────────────────────
/** Symbols → complex baseband at `fs` (a multiple of 4800): impulses through the RRC (α = 0.5, 8 symbols, scaled so a
 *  steady symbol reads its own value), then FM at 800 Hz a unit, on a carrier `offHz` from the dial. Unit amplitude. */
inline std::vector<cf32> modulate(const std::vector<int8_t>& syms, int fs = 48000, double offHz = 0.0, double* phase = nullptr) {
    const int sps = fs / kSymRate;
    const std::vector<float> h = rrcTaps(sps, 0.5, 8, double(sps));
    std::vector<float> up(syms.size() * size_t(sps) + h.size(), 0.0f);
    for (size_t k = 0; k < syms.size(); ++k) up[k * size_t(sps)] = float(syms[k]);
    std::vector<cf32> out(syms.size() * size_t(sps));
    double ph = phase ? *phase : 0.0;
    const size_t half = h.size() / 2;
    for (size_t i = 0; i < out.size(); ++i) {
        double f = 0;
        const size_t c = i + half;   // ★ the filter's delay taken out, so symbol k peaks at sample k·sps
        for (size_t j = 0; j < h.size(); ++j) if (c >= j && c - j < up.size()) f += double(h[j]) * up[c - j];
        ph += 2 * M_PI * (f * kDevPerUnit + offHz) / fs;
        if (ph > M_PI) ph -= 2 * M_PI; else if (ph < -M_PI) ph += 2 * M_PI;
        out[i] = cf32(float(std::cos(ph)), float(std::sin(ph)));
    }
    if (phase) *phase = ph;
    return out;
}

/** ★ A whole voice transmission as symbols: preamble, LSF, `frames` stream frames of Codec 2 3200 (`speech8k` encoded
 *  through the system's libcodec2 — silence when it is shorter, or when there is no library), the last frame flagged,
 *  then the EoT marker. `meta` (14 bytes, optional) is the META of every superframe; `metaKind` its subtype. */
inline std::vector<int8_t> voiceTransmission(const std::string& dst, const std::string& src, int frames,
                                             const std::vector<short>& speech8k, int can = 0,
                                             const uint8_t* meta = nullptr, int metaKind = 0,
                                             const std::vector<std::vector<uint8_t>>* metaBlocks = nullptr) {
    const vibe::freedv::Vocoder& v = vibe::freedv::vocoder();
    vibe::freedv::C2Opaque* c2 = v.ok ? v.create(vibe::freedv::kC2Mode3200) : nullptr;
    Lsf lsf = makeLsf(dst, src, streamType(2, metaKind, can), meta);
    std::vector<int8_t> s = preamble(true);
    const std::vector<int8_t> l = lsfFrame(lsf);
    s.insert(s.end(), l.begin(), l.end());
    for (int fn = 0; fn < frames; ++fn) {
        // ★ META may change only at a superframe boundary (§3.9.1): a new block at each one, round robin
        if (metaBlocks && !metaBlocks->empty() && fn % 6 == 0) {
            const auto& mb = (*metaBlocks)[size_t(fn / 6) % metaBlocks->size()];
            lsf = makeLsf(dst, src, streamType(2, metaKind, can), mb.data());
        }
        uint8_t pl[16] = {};
        for (int h = 0; h < 2; ++h) {
            short blk[160] = {};
            const size_t at = size_t(fn) * 320 + size_t(h) * 160;
            for (size_t i = 0; i < 160; ++i) blk[i] = at + i < speech8k.size() ? speech8k[at + i] : 0;
            if (c2) v.encode(c2, pl + 8 * h, blk);
        }
        const uint16_t f = uint16_t(fn & 0x7FFF) | (fn == frames - 1 ? 0x8000 : 0);
        const std::vector<int8_t> fr = streamFrame(lsf, fn % 6, f, pl);
        s.insert(s.end(), fr.begin(), fr.end());
    }
    const std::vector<int8_t> e = eotMarker();
    s.insert(s.end(), e.begin(), e.end());
    if (c2) v.destroy(c2);
    return s;
}
/** ★ A packet transmission: preamble, LSF (packet), the frames, EoT. `data` begins with the protocol byte (0x05 = SMS). */
inline std::vector<int8_t> packetTransmission(const std::string& dst, const std::string& src, const std::vector<uint8_t>& data, int can = 0) {
    const Lsf lsf = makeLsf(dst, src, packetType(can), nullptr);
    std::vector<int8_t> s = preamble(true);
    const std::vector<int8_t> l = lsfFrame(lsf);
    s.insert(s.end(), l.begin(), l.end());
    for (const auto& f : packetFrames(data)) s.insert(s.end(), f.begin(), f.end());
    const std::vector<int8_t> e = eotMarker();
    s.insert(s.end(), e.begin(), e.end());
    return s;
}
inline std::vector<uint8_t> smsPacket(const std::string& text) {
    std::vector<uint8_t> d{0x05};
    d.insert(d.end(), text.begin(), text.end());
    d.push_back(0);   // ★ null-terminated (Table "Reserved Packet Protocol Identifiers")
    return d;
}

// ── 1. The demodulator ────────────────────────────────────────────────────────────────────────────────────────────────
class Demod {
public:
    static constexpr int kFs = 24000, kSps = kFs / kSymRate;            // 5 samples a symbol
    static constexpr int kFrameSamples = kFrameSyms * kSps;             // 960
    struct Frame {
        Sync type = Sync::None;
        float sym[184] = {};      ///< the payload's soft symbols, in symbol units (±1, ±3)
        float sigma = 0;          ///< the rms distance of the symbols from the nearest ideal one
        float rho = 0;            ///< the sync correlation (1 = perfect)
        float offHz = 0;          ///< the carrier's distance from the dial (the discriminator's DC)
        bool flywheel = false;    ///< taken where the sync was expected, without finding it
    };
    enum class Verdict { Reject, Track, End };
    using Sink = std::function<Verdict(const Frame&)>;

    explicit Demod(Sink sink) : sink_(std::move(sink)),
        chI_(lowpassTaps(kFs, 6500, 49)), chQ_(lowpassTaps(kFs, 6500, 49)),
        fm_(float(kFs / (2 * M_PI * kDevPerUnit))), rrc_(rrcTaps(kSps, 0.5, 8, 1.0)) {
        for (int p = 0; p < 3; ++p) {
            const int8_t* s = syncSymbols(p == 0 ? Sync::Lsf : p == 1 ? Sync::Packet : Sync::Eot);
            double m = 0; for (int k = 0; k < 8; ++k) m += s[k]; m /= 8;
            double e = 0; for (int k = 0; k < 8; ++k) { pat_[p][k] = float(s[k] - m); e += pat_[p][k] * pat_[p][k]; }
            patNorm_[p] = float(std::sqrt(e));
        }
    }

    /** The channel's complex baseband at any rate, the dial at DC. */
    void feedIq(const cf32* x, int n, double rateHz) {
        if (n <= 0 || !(rateHz > 0)) return;
        if (!rsI_ || rsRate_ != rateHz) {
            rsI_.reset(new vibedsp::RationalResampler(rateHz, kFs, vibedsp::RationalResampler::ExactRate{}));
            rsQ_.reset(new vibedsp::RationalResampler(rateHz, kFs, vibedsp::RationalResampler::ExactRate{}));
            rsRate_ = rateHz;
        }
        re_.resize(size_t(n)); im_.resize(size_t(n));
        for (int i = 0; i < n; ++i) { re_[size_t(i)] = x[i].real(); im_[size_t(i)] = x[i].imag(); }
        const int mo = rsI_->maxOut(n);
        a_.resize(size_t(mo)); b_.resize(size_t(mo));
        const int na = rsI_->process(re_.data(), n, a_.data());
        const int nb = rsQ_->process(im_.data(), n, b_.data());
        const int m = std::min(na, nb);
        feed24k(a_.data(), b_.data(), m);
    }
    /** I and Q already at 24 kHz (the tests' fast path). */
    void feed24k(const float* i, const float* q, int n) {
        if (n <= 0) return;
        fi_.resize(size_t(n)); fq_.resize(size_t(n));
        const int ni = chI_.process(i, n, fi_.data());
        chQ_.process(q, n, fq_.data());
        z_.resize(size_t(ni));
        for (int k = 0; k < ni; ++k) z_[size_t(k)] = cf32(fi_[size_t(k)], fq_[size_t(k)]);
        d_.resize(size_t(ni));
        fm_.process(z_.data(), d_.data(), ni);
        f_.resize(size_t(ni));
        const int nf = rrc_.process(d_.data(), ni, f_.data());
        for (int k = 0; k < nf; ++k) { float v = f_[size_t(k)]; if (!std::isfinite(v)) v = 0; y_.push_back(v); }
        run();
        // ★ bounded: two frames of history is all the extraction and the search ever look back
        const long keep = 3L * kFrameSamples;
        if (long(y_.size()) > keep + 4096) { const long drop = long(y_.size()) - keep; y_.erase(y_.begin(), y_.begin() + drop); base_ += drop; }
    }
    bool tracking() const { return lock_; }
    /** Sync candidates looked at, and how many the receiver kept — the noise test's view of the search. */
    long candidates() const { return cands_; }

private:
    float at(long abs) const {
        const long i = abs - base_;
        return (i >= 0 && i < long(y_.size())) ? y_[size_t(i)] : 0.0f;
    }
    /** Normalised correlation of pattern `p` with the 8 symbols ending at sample `pos`. Signed: −1 = the inverse burst. */
    float rho(int p, long pos) const {
        float y[8], m = 0;
        for (int k = 0; k < 8; ++k) { y[k] = at(pos - long(7 - k) * kSps); m += y[k]; }
        m /= 8;
        float c = 0, e = 0;
        for (int k = 0; k < 8; ++k) { const float v = y[k] - m; c += v * pat_[p][k]; e += v * v; }
        if (e <= 1e-12f) return 0;
        return c / (patNorm_[p] * std::sqrt(e));
    }
    /** Cubic (Catmull-Rom) interpolation at a fractional absolute sample position. */
    float interp(double t) const {
        const long i = long(std::floor(t));
        const float u = float(t - double(i));
        const float p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        return p1 + 0.5f * u * (p2 - p0 + u * (2 * p0 - 5 * p1 + 4 * p2 - p3 + u * (3 * (p1 - p2) + p3 - p0)));
    }
    static Sync typeOf(int p, float r) {
        if (p == 0) return r > 0 ? Sync::Lsf : Sync::Stream;
        if (p == 1) return r > 0 ? Sync::Packet : Sync::Bert;
        return r > 0 ? Sync::Eot : Sync::None;
    }
    static float nearestSym(float z) { return z >= 2 ? 3.f : z >= 0 ? 1.f : z >= -2 ? -1.f : -3.f; }

    struct Pending { Sync type; double pos; float rho; bool fly; };

    /** ★ One sample position at a time in the search, with every candidate whose frame has arrived decoded before the
     *  next position is looked at — so a block of any size (the tests feed whole transmissions at once) is handled exactly
     *  as the server's 10 ms blocks are, and the candidate list stays short. */
    void run() {
        const long end = base_ + long(y_.size());   // one past the newest sample
        if (scan_ < base_ + 7 * kSps + 2) scan_ = base_ + 7 * kSps + 2;
        for (;;) {
            if (ready(end)) { decodeFront(); continue; }
            if (!lock_) {
                if (scan_ >= end) break;
                searchAt(scan_++);
                continue;
            }
            // ── following a transmission: the next burst, a few samples either side of where it should be ──
            if (!pend_.empty() || !(expect_ + kWin + 2 < double(end))) break;
            lookForNext();
        }
    }
    bool ready(long end) const { return !pend_.empty() && pend_.front().pos + double(kSps) * 184 + 4 < double(end); }
    /** The search at one position: the LSF/stream and packet bursts (EoT only ends a transmission, BERT is not decoded). */
    void searchAt(long p) {
        for (int q = 0; q < 2; ++q) {
            const float r0 = rho(q, p - 2), r1 = rho(q, p - 1), r2 = rho(q, p);
            const float a1 = std::fabs(r1);
            if (a1 >= kSearchRho && a1 >= std::fabs(r0) && a1 > std::fabs(r2)) {
                const Sync t = typeOf(q, r1);
                if (t == Sync::Bert) continue;   // ★ an instrument, not traffic
                ++cands_;
                /* ★★ THE LIST IS LONG, AND THE WEAKEST GOES (found in the first over-the-air test, 2026-11-16): a frame waits
                 *  40 ms for its samples, and its own random-looking payload throws up ~10 chance matches meanwhile. With
                 *  eight places, oldest out, the real LSF was pushed out by its own payload every time. */
                if (pend_.size() >= kMaxPending) {
                    auto w = std::min_element(pend_.begin(), pend_.end(), [](const Pending& x, const Pending& y) { return std::fabs(x.rho) < std::fabs(y.rho); });
                    if (std::fabs(w->rho) >= a1) continue;
                    pend_.erase(w);
                }
                pend_.push_back({t, double(p - 1) + parabola(std::fabs(r0), a1, std::fabs(r2)), r1, false});
            }
        }
    }
    void lookForNext() {
        const long c = long(std::lround(expect_));
        int bestQ = -1; long bestP = c; float best = 0;
        for (long p = c - kWin; p <= c + kWin; ++p) {
            for (int q = 0; q < 3; ++q) {
                const float r = rho(q, p);
                const Sync t = typeOf(q, r);
                if (t != want_ && t != Sync::Eot) continue;
                if (std::fabs(r) > best) { best = std::fabs(r); bestQ = q; bestP = p; }
            }
        }
        if (bestQ >= 0 && best >= kLockRho) {
            const float r1 = rho(bestQ, bestP);
            const Sync t = typeOf(bestQ, r1);
            if (t == Sync::Eot) {   // ★ the end-of-transmission marker: tell the receiver, search again after it
                Frame fr; fr.type = Sync::Eot; fr.rho = r1;
                sink_(fr);
                endTx(bestP + kFrameSamples);
                return;
            }
            const float r0 = std::fabs(rho(bestQ, bestP - 1)), r2 = std::fabs(rho(bestQ, bestP + 1));
            pend_.push_back({t, double(bestP) + parabola(r0, best, r2), r1, false});
        } else {
            pend_.push_back({want_, expect_, 0.f, true});   // ★ flywheel: the burst was lost in a fade, the frame may not be
        }
    }
    void decodeFront() {
        const Pending pd = pend_.front();
        pend_.pop_front();
        Frame fr;
        if (!extract(pd, fr)) { if (lock_) endTx(long(pd.pos) + 1); return; }
        const Verdict v = sink_(fr);
        if (v == Verdict::Track) {
            const bool was = lock_;
            lock_ = true;
            want_ = (fr.type == Sync::Lsf) ? lsfNext_ : fr.type;
            expect_ = pd.pos + kFrameSamples;
            pend_.clear();
            if (!was) scan_ = std::max(scan_, long(pd.pos) + 1);
        } else if (v == Verdict::End) {
            endTx(long(pd.pos) + kSps * 184);
        } else if (lock_) {
            endTx(long(pd.pos) + 1);   // ★ the receiver has given up on this transmission: search again from here
        }
    }
    void endTx(long from) {
        lock_ = false; pend_.clear();
        scan_ = std::max(scan_, from);
    }
    static double parabola(float a, float b, float c) {
        const float den = a - 2 * b + c;
        if (std::fabs(den) < 1e-9f) return 0;
        return std::max(-0.5, std::min(0.5, double(0.5f * (a - c) / den)));
    }
    bool extract(const Pending& pd, Frame& fr) {
        // symbol k (0..191) of the frame at pos − 35 + 5k: the 8 sync symbols, then the 184 of the payload
        float y[kFrameSyms];
        const double t0 = pd.pos - 7.0 * kSps;
        for (int k = 0; k < kFrameSyms; ++k) y[k] = interp(t0 + double(k) * kSps);
        const int8_t* s = syncSymbols(pd.type);
        if (!s) return false;
        // level and DC from the burst (a least-squares line through the known symbols)…
        double sy = 0, sp = 0, spp = 0, syp = 0;
        for (int k = 0; k < 8; ++k) { sy += y[k]; sp += s[k]; spp += double(s[k]) * s[k]; syp += double(y[k]) * s[k]; }
        double den = 8 * spp - sp * sp;
        double g = den > 0 ? (8 * syp - sp * sy) / den : 0, d = (sy - g * sp) / 8;
        if (!(g > 0.05)) return false;
        // …then refined from every symbol, decided (one pass is enough at the SNRs that decode)
        for (int it = 0; it < 2; ++it) {
            double a = 0, b = 0, aa = 0, ab = 0;
            for (int k = 0; k < kFrameSyms; ++k) {
                const double ideal = k < 8 ? s[k] : nearestSym(float((y[k] - d) / g));
                a += ideal; b += y[k]; aa += ideal * ideal; ab += ideal * y[k];
            }
            den = kFrameSyms * aa - a * a;
            if (den <= 0) break;
            const double g2 = (kFrameSyms * ab - a * b) / den;
            if (!(g2 > 0.05)) break;
            g = g2; d = (b - g * a) / kFrameSyms;
        }
        double e = 0;
        for (int k = 0; k < kFrameSyms; ++k) {
            const float z = float((y[k] - d) / g);
            if (k >= 8) fr.sym[k - 8] = z;
            const float r = z - (k < 8 ? float(s[k]) : nearestSym(z));
            e += double(r) * r;
        }
        fr.type = pd.type;
        fr.sigma = float(std::sqrt(e / kFrameSyms));
        fr.rho = pd.rho;
        fr.offHz = float(d * kDevPerUnit);
        fr.flywheel = pd.fly;
        return true;
    }

public:
    /** What follows an LSF the receiver accepted: stream frames, or packet frames. */
    void setAfterLsf(Sync s) { lsfNext_ = s; }

private:
    static constexpr float kSearchRho = 0.85f, kLockRho = 0.55f;
    static constexpr int kWin = 3;
    static constexpr size_t kMaxPending = 48;
    Sink sink_;
    std::unique_ptr<vibedsp::RationalResampler> rsI_, rsQ_;
    double rsRate_ = 0;
    vibedsp::RealFir chI_, chQ_;
    vibedsp::FmDemod fm_;
    vibedsp::RealFir rrc_;
    std::vector<float> re_, im_, a_, b_, fi_, fq_, d_, f_;
    std::vector<cf32> z_;
    std::vector<float> y_;
    long base_ = 0, scan_ = 0;
    float pat_[3][8] = {}, patNorm_[3] = {};
    bool lock_ = false;
    Sync want_ = Sync::Stream, lsfNext_ = Sync::Stream;
    double expect_ = 0;
    std::deque<Pending> pend_;
    long cands_ = 0;
};

// ── 2. The receiver: frames → transmissions, voice, data, the report ──────────────────────────────────────────────────
class Receiver {
public:
    /** Speech out: 8 kHz frames (320 samples a 40 ms frame; zeros for a frame that could not be decoded). */
    using SpeechSink = std::function<void(const short*, int)>;

    explicit Receiver(SpeechSink sp = nullptr) : speech_(std::move(sp)), demod_([this](const Demod::Frame& f) { return onFrame(f); }) {}
    ~Receiver() {
        const auto& v = vibe::freedv::vocoder();
        if (c3200_) v.destroy(c3200_);
        if (c1600_) v.destroy(c1600_);
    }
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void feedIq(const cf32* x, int n, double rate) { demod_.feedIq(x, n, rate); streamSec_ += double(n) / rate; }
    void feed24k(const float* i, const float* q, int n) { demod_.feed24k(i, q, n); streamSec_ += double(n) / Demod::kFs; }

    struct Sms { int id = 0; std::string src, dst, text; int ptype = 0; int bytes = 0; };
    struct Counts {
        int lsfOk = 0, lsfBad = 0, stream = 0, streamBad = 0, packets = 0, packetBad = 0, lichLsf = 0, voiceFrames = 0;
        int transmissions = 0, frames = 0;
        long candidates = 0;
    };
    const Counts& counts() const { return n_; }
    /** ★ The tests: every stream frame's frame number and 16 payload bytes as decoded (compared with what was sent). */
    void setPayloadSink(std::function<void(int, const uint8_t*)> f) { payload_ = std::move(f); }
    Counts countsNow() { n_.candidates = demod_.candidates(); return n_; }
    bool inSync() const { return state_ == St::Stream || state_ == St::Packet; }
    std::string src() const { return srcS_; }
    std::string dst() const { return dstS_; }
    bool lsfKnown() const { return lsfOk_; }
    int dataType() const { return typeKnown_ ? (type_ >> 1) & 3 : 0; }
    std::string metaText() const { return text_; }
    const Gnss* gnss() const { return gnssOk_ ? &gnss_ : nullptr; }
    std::string via() const { return via_; }
    const std::deque<Sms>& messages() const { return msgs_; }
    double ber() const { return ber_; }
    float offHz() const { return off_; }

    /** {"type":"m17", …} — see docs/v12/M17.md "The protocol". `version` moves when it changes. */
    std::string statusJson(uint64_t* version) {
        publish();
        if (version) *version = statusVer_;
        return status_;
    }

private:
    enum class St { Search, Tentative, Stream, Packet, PacketJoin };
    static constexpr int kLsfMaxErr = 60;      // channel bits corrected, of 368 — beyond this the LSF is not believed
    static constexpr int kStreamMaxErr = 34;   // of 272 (12.5 %): a frame decoded beyond this is not played
    static constexpr int kJoinMaxErr = 22;     // a late join's first frame must be cleaner still
    static constexpr int kPacketMaxErr = 50;   // of 368
    static constexpr int kJoinPacketMaxErr = 24;   // a packet joined without its LSF must be cleaner
    static constexpr int kMaxBadRun = 5;       // 200 ms of undecodable frames: the transmission has gone
    static constexpr float kMaxSigma = 0.75f;  // a frame whose symbols sit this far from the grid is not M17

    static std::string esc(const std::string& s) {
        std::string o;
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
            else if (c < 0x20 || c == 0x7F) o += ' ';
            else o += char(c);
        }
        return o;
    }
    /** Keep only text that is valid UTF-8 and printable: META and SMS come off the air. */
    static std::string cleanText(const uint8_t* p, size_t n) {
        std::string s;
        for (size_t i = 0; i < n; ) {
            const uint8_t c = p[i];
            if (c == 0) break;
            int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            if (len == 0 || i + size_t(len) > n) { s += '?'; ++i; continue; }
            bool ok = true;
            for (int k = 1; k < len; ++k) if ((p[i + size_t(k)] & 0xC0) != 0x80) ok = false;
            if (!ok) { s += '?'; ++i; continue; }
            if (len == 1 && (c < 0x20 || c == 0x7F)) s += ' ';
            else s.append(reinterpret_cast<const char*>(p + i), size_t(len));
            i += size_t(len);
        }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    void newTransmission() {
        lsfOk_ = typeKnown_ = false; type_ = 0;
        for (int i = 0; i < 6; ++i) { lichHave_[i] = lichPrevHave_[i] = false; }
        text_.clear(); textMask_ = 0; textNeed_ = 0; std::memset(textBlk_, ' ', sizeof textBlk_);
        gnssOk_ = false; via_.clear();
        srcS_.clear(); dstS_.clear(); can_ = -1;
        lastFn_ = -1; badRun_ = 0; good_ = 0;
        pkt_.clear(); pktNext_ = 0;
        errSum_ = 0; errBits_ = 0;
        confirmed_ = false;
        txStart_ = streamSec_;
    }
    void endTransmission() {
        if (!srcS_.empty()) { lastSrc_ = srcS_; lastDst_ = dstS_; }
        state_ = St::Search;
        dirty_ = true;
    }
    void adoptLsf(const Lsf& l) {
        lsf_ = l; lsfOk_ = true; typeKnown_ = true; type_ = l.type();
        srcS_ = decodeCallsign(l.src()); dstS_ = decodeCallsign(l.dst()); can_ = l.can();
        readMeta(l);
        dirty_ = true;
    }
    void readMeta(const Lsf& l) {
        if (!l.stream() || l.encType() != 0) return;
        const uint8_t* m = l.meta();
        bool empty = true; for (int i = 0; i < 14; ++i) if (m[i]) empty = false;
        if (empty) return;
        switch (l.encSub()) {
            case 0: {   // ★ text: up to four 13-byte blocks, the control byte saying which and how many
                const int need = m[0] >> 4, which = m[0] & 0xF;
                if (!need || !which || (which & (which - 1))) break;
                const int blk = which == 1 ? 0 : which == 2 ? 1 : which == 4 ? 2 : 3;
                std::memcpy(textBlk_[blk], m + 1, 13);
                textMask_ |= which; textNeed_ = need;
                if ((textMask_ & textNeed_) == textNeed_) {
                    uint8_t all[52]; int nb = 0;
                    for (int b = 0; b < 4; ++b) if (textNeed_ & (1 << b)) { std::memcpy(all + 13 * nb, textBlk_[b], 13); ++nb; }
                    const std::string t = cleanText(all, size_t(13 * nb));
                    if (t != text_) { text_ = t; dirty_ = true; }
                }
                break;
            }
            case 1: { const Gnss g = parseGnss(m); if (g.posValid) { gnss_ = g; gnssOk_ = true; dirty_ = true; } break; }
            case 2: {   // ★ the extended callsign: who is really talking through this repeater, and from which reflector
                const std::string a = decodeCallsign(addrFrom(m)), b = decodeCallsign(addrFrom(m + 6));
                const std::string v = b.empty() ? a : a + " via " + b;
                if (v != via_) { via_ = v; dirty_ = true; }
                break;
            }
            default: break;
        }
    }
    /** ★ A late joiner's LSF, chunk by chunk from the LICH. The chunks with META (2–5) may change at each superframe, so
     *  each counter keeps its newest chunk and the one before; every combination is tried against the CRC — a chunk
     *  damaged in this superframe is replaced by last superframe's (the LICH combined across superframes). */
    void lichChunk(int cnt, const uint8_t* c5) {
        if (lichHave_[cnt] && std::memcmp(lich_[cnt], c5, 5) == 0) return;
        if (lichHave_[cnt]) { std::memcpy(lichPrev_[cnt], lich_[cnt], 5); lichPrevHave_[cnt] = true; }
        std::memcpy(lich_[cnt], c5, 5); lichHave_[cnt] = true;
        // ★ TYPE lives in bytes 12–13 — chunk 2 — so the voice can start before the whole LSF is in
        if (cnt == 2 && !typeKnown_) { type_ = uint16_t((c5[2] << 8) | c5[3]); typeKnown_ = true; dirty_ = true; }
        for (int i = 0; i < 6; ++i) if (!lichHave_[i]) return;
        for (int m = 0; m < 64; ++m) {
            Lsf l;
            bool ok = true;
            for (int i = 0; i < 6; ++i) {
                const bool prev = (m >> i) & 1;
                if (prev && !lichPrevHave_[i]) { ok = false; break; }
                std::memcpy(l.b + 5 * i, prev ? lichPrev_[i] : lich_[i], 5);
            }
            if (!ok || !l.crcOk()) continue;
            if (!lsfOk_ || std::memcmp(l.b, lsf_.b, 30) != 0) {
                if (!lsfOk_) ++n_.lichLsf;
                adoptLsf(l);
            }
            return;
        }
    }

    /** ★ DIAGNOSTIC: VIBE_M17_LOG=1 prints every frame the receiver is handed to stderr (stream time, type, sync
     *  correlation, the symbols' distance from the grid, the channel errors the FEC corrected, the receiver's state).
     *  Off unless set. */
    void note(const char* what, const Demod::Frame& f, int errs) const {
        static const bool on = [] { const char* e = std::getenv("VIBE_M17_LOG"); return e && e[0] && e[0] != '0'; }();
        if (!on) return;
        static const char* const kT[] = { "none", "LSF", "stream", "packet", "BERT", "EoT" };
        static const char* const kS[] = { "search", "tentative", "stream", "packet", "pkt-join" };
        std::fprintf(stderr, "[m17 %p] %8.3f s %-6s %s rho %+.2f sigma %.2f off %+5.0f Hz errs %3d %-9s %s\n", static_cast<const void*>(this),
                     streamSec_, kT[int(f.type)], f.flywheel ? "fly " : "sync", f.rho, f.sigma, f.offHz, errs, kS[int(state_)], what);
    }

    Demod::Verdict onFrame(const Demod::Frame& f) {
        const Demod::Verdict v = onFrameInner(f);
        return v;
    }
    Demod::Verdict onFrameInner(const Demod::Frame& f) {
        if (f.type == Sync::Eot) { note("end marker", f, 0); if (state_ != St::Search) endTransmission(); return Demod::Verdict::End; }
        if (f.sigma > kMaxSigma && !(f.flywheel && inSync())) { note("off the grid", f, -1); return state_ == St::Search ? Demod::Verdict::Reject : badFrame(); }
        int8_t t3[kPayloadBits];
        softBits(f.sym, 30.0f, t3);
        switch (f.type) {
            case Sync::Lsf: {
                const Decoded d = decodeLsf(t3);
                if (d.errors > kLsfMaxErr) { note("LSF too damaged", f, d.errors); ++n_.lsfBad; return Demod::Verdict::Reject; }
                Lsf l; std::memcpy(l.b, d.bytes.data(), 30);
                if (!l.crcOk()) { note("LSF CRC fails", f, d.errors); ++n_.lsfBad; return Demod::Verdict::Reject; }
                note("LSF", f, d.errors);
                ++n_.lsfOk; ++n_.frames;
                if (inSync() && state_ == St::Stream) endTransmission();
                newTransmission();
                adoptLsf(l);
                confirm();
                state_ = l.stream() ? St::Stream : St::Packet;
                demodAfterLsf(l.stream() ? Sync::Stream : Sync::Packet);
                noteErr(d.errors, kPayloadBits);
                off_ = f.offHz;
                return Demod::Verdict::Track;
            }
            case Sync::Stream: return streamFrame(f, t3);
            case Sync::Packet: return packetFrame(f, t3);
            default: return state_ == St::Search ? Demod::Verdict::Reject : badFrame();
        }
    }
    void demodAfterLsf(Sync s) { demod_.setAfterLsf(s); }
    Demod::Verdict badFrame() {
        ++badRun_;
        if (state_ == St::Stream) emitSilence();
        if (badRun_ >= kMaxBadRun || state_ != St::Stream) { endTransmission(); return Demod::Verdict::Reject; }
        return Demod::Verdict::Track;
    }
    void noteErr(int e, int bits) {
        errSum_ += e; errBits_ += bits;
        if (errBits_ >= 272 * 25 || !berInit_) {   // ★ the report's error rate moves about once a second
            ber_ = errBits_ ? 100.0 * double(errSum_) / double(errBits_) : 0;
            berInit_ = true; errSum_ = errBits_ = 0;
        }
    }

    Demod::Verdict streamFrame(const Demod::Frame& f, const int8_t* t3) {
        uint8_t lich[6]; int cnt = -1, gerr = 0;
        const bool lichOk = decodeLich(t3, lich, &cnt, &gerr);
        const Decoded d = decodeStreamPayload(t3);
        const int fn = (d.bytes[0] << 8) | d.bytes[1];
        if (state_ == St::Search) {
            // ★ a late join: believed only with a clean LICH and a clean payload, and played from the next frame on
            if (f.flywheel || !lichOk || gerr > 6 || d.errors > kJoinMaxErr) { note("not a stream frame", f, d.errors); ++n_.streamBad; return Demod::Verdict::Reject; }
            note("late join?", f, d.errors);
            newTransmission();
            state_ = St::Tentative;
            lastFn_ = fn & 0x7FFF;
            lichChunk(cnt, lich);
            if (fn & 0x8000) { endTransmission(); return Demod::Verdict::End; }
            return Demod::Verdict::Track;
        }
        if (state_ == St::Tentative) {
            if (!lichOk || d.errors > kStreamMaxErr || (fn & 0x7FFF) != ((lastFn_ + 1) & 0x7FFF)) {
                note("late join refused", f, d.errors);
                ++n_.streamBad; endTransmission(); return Demod::Verdict::Reject;
            }
            state_ = St::Stream;   // ★ two frames in a row, numbered in order, 40 ms apart: this is M17
            confirm();
        } else if (state_ != St::Stream) {
            note("stream frame in a packet", f, d.errors);
            return badFrame();
        }
        if (d.errors > kStreamMaxErr) { note("too damaged", f, d.errors); ++n_.streamBad; noteErr(d.errors, 272); return badFrame(); }
        note("stream", f, d.errors);
        badRun_ = 0; ++n_.stream; ++n_.frames;
        noteErr(d.errors, 272);
        off_ = f.offHz;
        if (lichOk) lichChunk(cnt, lich);
        lastFn_ = fn & 0x7FFF;
        if (payload_) payload_(fn, d.bytes.data() + 2);
        voice(d.bytes.data() + 2);
        if (fn & 0x8000) { endTransmission(); return Demod::Verdict::End; }
        return Demod::Verdict::Track;
    }
    /** Once per transmission, when it is believed: an LSF with a good CRC, a late join's second frame, a packet's CRC. */
    void confirm() {
        if (confirmed_) return;
        confirmed_ = true; ++n_.transmissions; dirty_ = true;
    }

    void emitSilence() { if (speech_) { static const short z[320] = {}; speech_(z, 320); } }
    void voice(const uint8_t* p16) {
        if (!speech_) return;
        const auto& v = vibe::freedv::vocoder();
        const int dt = typeKnown_ ? (type_ >> 1) & 3 : 0;
        const int enc = typeKnown_ ? (type_ >> 3) & 3 : 0;
        // ★ no voice until TYPE is known (the LSF, or LICH chunk 2 — at most 240 ms for a late joiner), none encrypted
        if (!v.ok || !typeKnown_ || enc != 0 || (dt != 2 && dt != 3)) { emitSilence(); return; }
        short out[320];
        if (dt == 2) {
            if (!c3200_) c3200_ = v.create(vibe::freedv::kC2Mode3200);
            if (!c3200_) { emitSilence(); return; }
            v.decode(c3200_, out, p16);
            v.decode(c3200_, out + 160, p16 + 8);
        } else {
            if (!v.mode1600) { emitSilence(); return; }
            if (!c1600_) c1600_ = v.create(vibe::freedv::kC2Mode1600);
            if (!c1600_) { emitSilence(); return; }
            v.decode(c1600_, out, p16);   // ★ the other 8 bytes are the data channel (no application defined for them)
        }
        ++n_.voiceFrames;
        speech_(out, 320);
    }

    Demod::Verdict packetFrame(const Demod::Frame& f, const int8_t* t3) {
        const Decoded d = decodePacket(t3);
        const bool joining = state_ == St::Search || state_ == St::PacketJoin;
        if (d.errors > (joining ? kJoinPacketMaxErr : kPacketMaxErr)) {
            note("packet too damaged", f, d.errors);
            ++n_.packetBad;
            if (state_ == St::Search) return Demod::Verdict::Reject;
            endTransmission(); return Demod::Verdict::Reject;   // ★ a packet with a frame missing cannot pass its CRC
        }
        const bool eof = d.bytes[25] & 0x80;
        const int cntr = (d.bytes[25] >> 2) & 0x1F;
        if (state_ == St::Search) {
            // ★ a packet whose LSF was missed: only from its first frame (or a one-frame packet), the source unknown, and
            //   believed only when its CRC passes
            if (f.flywheel || !(cntr == 0 || eof) || (d.bytes[25] & 3)) { note("not a packet frame", f, d.errors); ++n_.packetBad; return Demod::Verdict::Reject; }
            newTransmission();
            state_ = St::PacketJoin;
        } else if (state_ != St::Packet && state_ != St::PacketJoin) {
            note("packet frame in a stream", f, d.errors);
            return badFrame();
        }
        note("packet", f, d.errors);
        if (!eof) {
            if (cntr != pktNext_ || pkt_.size() > 800) { ++n_.packetBad; endTransmission(); return Demod::Verdict::Reject; }
            pkt_.insert(pkt_.end(), d.bytes.begin(), d.bytes.begin() + 25);
            ++pktNext_;
            return Demod::Verdict::Track;
        }
        const int used = std::max(1, std::min(25, cntr));
        pkt_.insert(pkt_.end(), d.bytes.begin(), d.bytes.begin() + used);
        if (pkt_.size() >= 3 && crc16(pkt_.data(), pkt_.size()) == 0) {
            confirm();
            ++n_.packets; n_.frames += pktNext_ + 1;
            Sms m; m.id = ++msgId_; m.src = srcS_; m.dst = dstS_; m.ptype = pkt_[0]; m.bytes = int(pkt_.size()) - 2;
            if (pkt_[0] == 0x05) m.text = cleanText(pkt_.data() + 1, pkt_.size() - 3);
            msgs_.push_back(m);
            while (msgs_.size() > 8) msgs_.pop_front();
            dirty_ = true;
        } else { note("packet CRC fails", f, d.errors); ++n_.packetBad; }
        endTransmission();
        return Demod::Verdict::End;
    }

    void publish() {
        const bool sync = state_ == St::Stream || state_ == St::Packet;
        const bool tick = streamSec_ - shownAt_ >= 1.0;
        if (!dirty_ && !tick && sync == wasSync_) return;
        if (tick) { shownAt_ = streamSec_; berShown_ = int(std::lround(ber_)); offShown_ = int(std::lround(off_ / 50.0) * 50); }
        dirty_ = false; wasSync_ = sync;
        std::string j = "{\"type\":\"m17\",\"state\":\"";
        j += sync ? "sync" : "search";
        j += "\"";
        if (sync) {
            const int dt = typeKnown_ ? (type_ >> 1) & 3 : 0;
            const bool pk = state_ == St::Packet;
            j += ",\"kind\":\""; j += pk ? "packet" : dt == 2 ? "voice" : dt == 3 ? "voicedata" : dt == 1 ? "data" : ""; j += "\"";
            if (!srcS_.empty()) j += ",\"src\":\"" + esc(srcS_) + "\"";
            if (!dstS_.empty()) j += ",\"dst\":\"" + esc(dstS_) + "\"";
            if (can_ >= 0) j += ",\"can\":" + std::to_string(can_);
            if (typeKnown_ && !pk) {
                const int enc = (type_ >> 3) & 3;
                if (enc == 1) j += ",\"enc\":\"scrambler\""; else if (enc == 2) j += ",\"enc\":\"aes\""; else if (enc == 3) j += ",\"enc\":\"other\"";
            }
            j += ",\"ber\":" + std::to_string(berShown_);
            j += ",\"off\":" + std::to_string(offShown_);
        } else if (!lastSrc_.empty()) {
            j += ",\"last\":\"" + esc(lastSrc_) + "\"";
        }
        if (!text_.empty()) j += ",\"text\":\"" + esc(text_) + "\"";
        if (!via_.empty()) j += ",\"via\":\"" + esc(via_) + "\"";
        if (gnssOk_) {
            char b[160];
            std::snprintf(b, sizeof b, ",\"gnss\":{\"lat\":%.5f,\"lon\":%.5f", gnss_.lat, gnss_.lon);
            j += b;
            if (gnss_.altValid) { std::snprintf(b, sizeof b, ",\"alt\":%.1f", gnss_.altM); j += b; }
            if (gnss_.velValid) { std::snprintf(b, sizeof b, ",\"spd\":%.1f,\"brg\":%d", gnss_.speedKmh, gnss_.bearing); j += b; }
            if (gnss_.radiusValid) j += ",\"r\":" + std::to_string(gnss_.radiusM);
            j += "}";
        }
        if (!msgs_.empty()) {
            const Sms& m = msgs_.back();
            j += ",\"msg\":{\"id\":" + std::to_string(m.id);
            if (!m.src.empty()) j += ",\"src\":\"" + esc(m.src) + "\"";
            if (!m.dst.empty()) j += ",\"dst\":\"" + esc(m.dst) + "\"";
            j += ",\"ptype\":" + std::to_string(m.ptype) + ",\"bytes\":" + std::to_string(m.bytes);
            if (m.ptype == 0x05) j += ",\"text\":\"" + esc(m.text) + "\"";
            j += "}";
        }
        j += "}";
        if (j != status_) { status_ = j; ++statusVer_; }
    }

    SpeechSink speech_;
    Demod demod_;
    St state_ = St::Search;
    Counts n_;
    Lsf lsf_;
    bool lsfOk_ = false, typeKnown_ = false;
    uint16_t type_ = 0;
    uint8_t lich_[6][5] = {}, lichPrev_[6][5] = {};
    bool lichHave_[6] = {}, lichPrevHave_[6] = {};
    std::string srcS_, dstS_, lastSrc_, lastDst_;
    int can_ = -1;
    int lastFn_ = -1, badRun_ = 0, good_ = 0;
    uint8_t textBlk_[4][13] = {};
    int textMask_ = 0, textNeed_ = 0;
    std::string text_, via_;
    Gnss gnss_; bool gnssOk_ = false;
    std::vector<uint8_t> pkt_; int pktNext_ = 0;
    std::deque<Sms> msgs_; int msgId_ = 0;
    long errSum_ = 0, errBits_ = 0; double ber_ = 0; bool berInit_ = false;
    float off_ = 0;
    double streamSec_ = 0, txStart_ = 0, shownAt_ = -1e9;
    int berShown_ = 0, offShown_ = 0;
    bool dirty_ = true, wasSync_ = false, confirmed_ = false;
    std::function<void(int, const uint8_t*)> payload_;
    std::string status_ = "{\"type\":\"m17\",\"state\":\"search\"}";
    uint64_t statusVer_ = 1;
    vibe::freedv::C2Opaque* c3200_ = nullptr;
    vibe::freedv::C2Opaque* c1600_ = nullptr;
};

/** ★ The one question every reader asks: is the mode offered? The vocoder loaded and runs Codec 2 3200. */
inline bool available() { return vibe::freedv::vocoder().ok; }

// ── 3. One listener's receiver on its own thread ──────────────────────────────────────────────────────────────────────
class M17Listener {
public:
    static constexpr int kOutRate = 48000;

    M17Listener() : up_(8000, kOutRate) { th_ = std::thread([this] { loop(); }); }
    ~M17Listener() {
        { std::lock_guard<std::mutex> lk(m_); stop_ = true; }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }
    M17Listener(const M17Listener&) = delete;
    M17Listener& operator=(const M17Listener&) = delete;

    /** The channel's complex baseband (RxPipeline::Callbacks::iq). Never blocks for long: a copy under a leaf mutex. */
    void feedIq(const cf32* x, int n, double rateHz) {
        if (n <= 0 || !(rateHz > 0)) return;
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(Block{std::vector<cf32>(x, x + n), rateHz});
        queued_ += size_t(n);
        while (queued_ > size_t(rateHz * 2.0) && q_.size() > 1) { queued_ -= q_.front().iq.size(); q_.pop_front(); ++dropped_; }
        cv_.notify_one();
    }
    /** Exactly `frames` frames of 48 kHz mono speech (zeros while there is none). */
    void takePcm(float* mono, int frames) {
        std::lock_guard<std::mutex> lk(pm_);
        if (!primed_ && pcm_.size() >= size_t(kPrimeFrames)) primed_ = true;
        int got = 0;
        if (primed_) {
            got = std::min<int>(frames, int(pcm_.size()));
            std::copy(pcm_.begin(), pcm_.begin() + got, mono);
            pcm_.erase(pcm_.begin(), pcm_.begin() + got);
            if (got < frames) { primed_ = false; ++underruns_; }
        }
        std::fill(mono + got, mono + frames, 0.0f);
    }
    std::string statusJson(uint64_t* version) const {
        std::lock_guard<std::mutex> lk(sm_);
        if (version) *version = statusVer_;
        return status_;
    }
    uint64_t droppedBlocks() const { std::lock_guard<std::mutex> lk(m_); return dropped_; }
    size_t queuedFrames() const { std::lock_guard<std::mutex> lk(m_); return queued_ + (busy_ ? 1 : 0); }
    uint64_t underruns() const { std::lock_guard<std::mutex> lk(pm_); return underruns_; }
    int voiceFrames() const { return voice_.load(std::memory_order_relaxed); }
    bool inSync() const { return sync_.load(std::memory_order_relaxed); }

private:
    static constexpr int kPrimeFrames = kOutRate / 5;        // 0.2 s — five M17 frames
    static constexpr int kMaxFrames = kOutRate * 3 / 2;      // 1.5 s, then the oldest goes (latency never grows)
    struct Block { std::vector<cf32> iq; double rate = 0; };

    void loop() {
        vibeAudioDecodeThread("vibe-m17");
        Receiver rx([this](const short* s, int n) { onSpeech(s, n); });
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_) break;
                b = std::move(q_.front()); q_.pop_front(); queued_ -= b.iq.size();
                busy_ = true;
            }
            rx.feedIq(b.iq.data(), int(b.iq.size()), b.rate);
            uint64_t v = 0;
            const std::string j = rx.statusJson(&v);
            voice_.store(rx.counts().voiceFrames, std::memory_order_relaxed);
            sync_.store(rx.inSync(), std::memory_order_relaxed);
            { std::lock_guard<std::mutex> lk(sm_); if (j != status_) { status_ = j; ++statusVer_; } }
            { std::lock_guard<std::mutex> lk(m_); busy_ = false; }
        }
    }
    void onSpeech(const short* s, int n) {
        f8_.resize(size_t(n));
        for (int i = 0; i < n; ++i) f8_[size_t(i)] = float(s[i]) * (1.0f / 32768.0f);
        o48_.resize(size_t(up_.maxOut(n)));
        const int no = up_.process(f8_.data(), n, o48_.data());
        std::lock_guard<std::mutex> lk(pm_);
        pcm_.insert(pcm_.end(), o48_.begin(), o48_.begin() + no);
        if (pcm_.size() > size_t(kMaxFrames)) pcm_.erase(pcm_.begin(), pcm_.end() - kPrimeFrames);
    }

    vibedsp::RationalResampler up_;
    std::vector<float> f8_, o48_;
    std::atomic<int> voice_{0};
    std::atomic<bool> sync_{false};

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Block> q_;
    size_t queued_ = 0;
    uint64_t dropped_ = 0;
    bool stop_ = false, busy_ = false;

    mutable std::mutex pm_;
    std::deque<float> pcm_;
    bool primed_ = false;
    uint64_t underruns_ = 0;

    mutable std::mutex sm_;
    std::string status_ = "{\"type\":\"m17\",\"state\":\"search\"}";
    uint64_t statusVer_ = 1;

    std::thread th_;
};

}  // namespace m17
}  // namespace vibe
