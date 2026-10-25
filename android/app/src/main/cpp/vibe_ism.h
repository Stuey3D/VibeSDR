// vibe_ism.h — V12 ISM-band sensor decoder: the generic FRONT END (docs/v12/ISM-DECODER.md).
//
// Whole sub-band capture (433.92 / 868.6 / 315 / 915 MHz) → a sparse FFT energy detector that finds each burst's
// frequency → one narrow channel per burst (mix + integrate-and-dump to ~250 kS/s, from a pre-roll ring so the start
// is not lost) → at the end of the burst, an OOK envelope slice and an FSK discriminator slice, both as signed run
// lengths in µs (+mark / −space; for FSK +high tone / −low tone). The protocol decoders (vibe_ism_protocols.h) take the
// runs; nothing here knows a protocol.
//
// ★★★ CLEAN ROOM. Written from protocol FACTS only — no rtl_433 or other GPL decoder source was read. The DSP here is
//     textbook (Hann-windowed FFT power, a minimum-tracking noise floor, a complex NCO, integrate-and-dump, a polar
//     discriminator, a hysteresis slicer).
// ★★ Header-only and links nothing, like vibe_adsb.h, so test-ism builds with one g++ line (scripts/run-tests.sh).
// ★★ CPU: the detector FFTs only one block in every kHopBlocks, so its cost is a fraction of a full-rate FFT; a channel
//    costs a mix and an add per capture sample, and only while a burst is on the air. Measured in test-ism (CPU row) and
//    quoted in ISM-DECODER.md §CPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vibeism {

using cf = std::complex<float>;
/** Signed run lengths, µs: + = mark (carrier on / high tone), − = space (carrier off / low tone). */
using Runs = std::vector<int>;

/** One burst, demodulated two ways. The decoders try both. */
struct Burst {
    double tSec = 0;          ///< capture time of the burst's first sample (seconds since FrontEnd start)
    double offHz = 0;         ///< channel centre relative to the capture centre
    float  levelDb = -99;     ///< envelope peak (95th percentile), dBFS
    float  noiseDb = -99;     ///< the channel's level before the burst, dBFS
    bool   clipped = false;   ///< the capture neared full scale during the burst
    Runs   ook;               ///< envelope slice (always present) — the finest of ookAll
    Runs   fsk;               ///< discriminator slice — empty when the envelope is not constant enough to be FSK
    /** ★ The same burst sliced at several smoothings (finest first): a 17 kbps FSK bit and a 1 kHz Oregon pulse want
     *  very different filters, and a weak slow burst only slices cleanly when smoothed to its own speed. The decoders
     *  try each (decodeAll). */
    std::vector<Runs> ookAll, fskAll;
    float  carrierHz = 0;     ///< the carrier (OOK) or the tones' midpoint (FSK), relative to the channel centre
    float  fskDevHz = 0;      ///< half the distance between the two tones (0 = not FSK)
    float  sampleUs = 0;      ///< the slicer's time resolution
};

/** A radix-2 FFT, forward, natural order (the DAB one's shape, kept local so this header links nothing). */
class SmallFft {
public:
    explicit SmallFft(int n) : n_(n), rev_(size_t(n)), tw_(size_t(n / 2)) {
        int bits = 0; while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) { int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev_[size_t(i)] = r; }
        for (int i = 0; i < n / 2; ++i) { const double a = -2.0 * M_PI * i / n; tw_[size_t(i)] = cf(float(std::cos(a)), float(std::sin(a))); }
    }
    void forward(cf* x) const {
        for (int i = 0; i < n_; ++i) { const int r = rev_[size_t(i)]; if (r > i) std::swap(x[i], x[r]); }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len >> 1, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k) {
                    const cf w = tw_[size_t(k * step)];
                    const cf u = x[i + k], v = x[i + k + half] * w;
                    x[i + k] = u + v; x[i + k + half] = u - v;
                }
        }
    }
private:
    int n_;
    std::vector<int> rev_;
    std::vector<cf> tw_;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE FRONT END
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class FrontEnd {
public:
    static constexpr int    kN = 256;            ///< detector FFT size (≈ 9.4 kHz bins at 2.4 MS/s)
    static constexpr int    kHopBlocks = 3;      ///< FFT one block of kN in every kHopBlocks — ~3 100 FFTs/s at 2.4 MS/s
    static constexpr double kChanRate = 250e3;   ///< a burst's channel rate (integrate-and-dump target)
    static constexpr double kPrerollS = 0.006;   ///< channel starts this far before the first detection
    static constexpr double kCloseS = 0.025;     ///< no energy for this long = the burst is over (PT2262's sync is ~15 ms)
    static constexpr double kMaxBurstS = 1.6;    ///< a carrier that never stops is cut here (and its floor rises)
    static constexpr double kMatchHz = 80e3;     ///< a detection this close to an open channel belongs to it (FSK's two tones)
    static constexpr int    kMaxChans = 4;
    float threshDb = 10.0f;                        ///< detection: a bin this far over its own noise floor

    explicit FrontEnd(double rateHz) : fft_(kN) {
        // ★ Pre-decimate anything above 2.6 MS/s (an Airspy at 10 MS/s) by an integer boxcar: the detector and the
        //   channels then cost the same whatever the radio runs at. The sensors sit near the middle, the boxcar's
        //   first null is at the new rate — plenty for a detector and a 250 kHz channel.
        pre_ = std::max(1, int(std::ceil(rateHz / 2.6e6)));
        w_ = rateHz / pre_;
        chanM_ = std::max(1, int(std::lround(w_ / kChanRate)));
        ring_.assign(size_t(1) << 18, cf(0, 0));      // ~109 ms at 2.4 MS/s
        win_.resize(kN);
        for (int i = 0; i < kN; ++i) win_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kN));
        floor_.assign(kN, 0.0f);
        pw_.assign(kN, 0.0f);
        fbuf_.resize(kN);
        hop_ = kN * kHopBlocks;
    }

    double workRate() const { return w_; }
    double chanRate() const { return w_ / chanM_; }
    uint64_t bursts() const { return bursts_; }
    /** The detector's median bin floor, dBFS per bin (for the status line and the gain rule). */
    double noiseDb() const { return noiseDb_; }
    /** Share of detector frames in the last second where any sample neared full scale — the gain rule's overload. */
    double clipFrac() const { return clipFrac_; }
    /** Ignore detections within this many Hz of the capture centre (the DC remnant). */
    void setDcGuardHz(double hz) { dcGuardHz_ = hz; }

    /** Feed capture samples. `onBurst(const Burst&)` is called on THIS thread when a burst ends. */
    template <class F> void feed(const cf* x, size_t n, F&& onBurst) {
        for (size_t i = 0; i < n; ++i) {
            if (pre_ > 1) { preAcc_ += x[i]; if (++preCnt_ < pre_) continue; pushWork(preAcc_ * (1.0f / pre_), onBurst); preAcc_ = 0; preCnt_ = 0; }
            else pushWork(x[i], onBurst);
        }
    }

    /** Close every open channel now (end of stream). */
    template <class F> void flush(F&& onBurst) {
        for (auto& c : ch_) if (c.on) closeChan(c, onBurst);
    }

private:
    struct Chan {
        bool on = false;
        double offHz = 0;
        cf rot{1, 0}, inc{1, 0};
        int mcnt = 0, renorm = 0;
        cf acc{0, 0};
        uint64_t next = 0;      ///< the next working sample (absolute) this channel has not yet consumed
        uint64_t start = 0, lastHit = 0;
        int hits = 0;           ///< detector frames that saw it — a noise spike is one; a burst is many
        float peakPow = 0, framePow = 0;   ///< its strongest bin power, ever and in the current detector frame
        float peakAbs = 0;
        std::vector<cf> bb;
    };

    template <class F> void pushWork(cf v, F& onBurst) {
        const size_t mask = ring_.size() - 1;
        ring_[size_t(total_ & mask)] = v;
        const float a = std::max(std::fabs(v.real()), std::fabs(v.imag()));
        if (a > blockPeak_) blockPeak_ = a;
        ++total_;
        // ★ channels consume up to "now" every 1024 samples (and at a detection), never more than the ring holds
        if ((total_ & 1023) == 0) for (auto& c : ch_) if (c.on) runChan(c, total_);
        if (++sinceFft_ >= hop_) { sinceFft_ = 0; detect(onBurst); }
    }

    void runChan(Chan& c, uint64_t upto) {
        const size_t mask = ring_.size() - 1;
        const float invM = 1.0f / chanM_;
        for (uint64_t s = c.next; s < upto; ++s) {
            const cf v = ring_[size_t(s & mask)];
            const float pa = std::max(std::fabs(v.real()), std::fabs(v.imag()));
            if (pa > c.peakAbs) c.peakAbs = pa;
            c.acc += v * c.rot;
            c.rot *= c.inc;
            if (++c.mcnt >= chanM_) {
                c.bb.push_back(c.acc * invM);
                c.acc = 0; c.mcnt = 0;
                if (++c.renorm >= 256) { c.renorm = 0; c.rot /= std::abs(c.rot); }
            }
        }
        c.next = upto;
    }

    template <class F> void detect(F& onBurst) {
        const size_t mask = ring_.size() - 1;
        const uint64_t s0 = total_ - kN;
        for (int i = 0; i < kN; ++i) fbuf_[size_t(i)] = ring_[size_t((s0 + i) & mask)] * win_[size_t(i)];
        fft_.forward(fbuf_.data());
        const float norm = 1.0f / (float(kN) * float(kN) * 0.375f);   // Hann's power gain, so a bin reads as dBFS-ish
        for (int i = 0; i < kN; ++i) pw_[size_t(i)] = std::norm(fbuf_[size_t(i)]) * norm;
        // ★ the overload meter: a block of capture that neared full scale (an 8-bit RTL clips at 1.0)
        clipWin_ += blockPeak_ > 0.85f ? 1 : 0; ++clipFrames_; blockPeak_ = 0;
        if (frames_ < 64) {
            // ★ the first frames SEED the floor fast (signals present at start would otherwise stay "noise" for a while)
            for (int i = 0; i < kN; ++i) floor_[size_t(i)] = frames_ == 0 ? pw_[size_t(i)] : floor_[size_t(i)] + 0.15f * (pw_[size_t(i)] - floor_[size_t(i)]);
        }
        ++frames_;
        const float thr = std::pow(10.0f, threshDb / 10.0f);
        const double binHz = w_ / kN;
        const double now = double(total_) / w_;
        // ★ bin index → signed frequency: 0..N/2-1 positive, N/2..N-1 negative
        auto binFreq = [&](int k) { return (k < kN / 2 ? k : k - kN) * binHz; };
        // groups of over-threshold bins, scanned in frequency order (negative first), one-bin gaps bridged
        int edge = int(kN * 0.05);
        struct G { double f, wsum; float peak, pmax; int width; };
        G groups[16]; int ng = 0;
        bool inG = false; int gapRun = 0; G cur{0, 0, 0, 0, 0};
        for (int j = 0; j < kN; ++j) {
            const int k = (j + kN / 2) % kN;                 // -N/2 .. N/2-1
            const int sk = k < kN / 2 ? k : k - kN;
            const bool usable = std::abs(sk) < kN / 2 - edge && std::fabs(binFreq(k)) > dcGuardHz_;
            const float ex = usable && floor_[size_t(k)] > 0 ? pw_[size_t(k)] / floor_[size_t(k)] : 0.0f;
            if (ex > thr) {
                if (!inG) { cur = G{0, 0, 0, 0, 0}; inG = true; }
                gapRun = 0;
                cur.f += binFreq(k) * pw_[size_t(k)]; cur.wsum += pw_[size_t(k)];
                cur.peak = std::max(cur.peak, ex); cur.pmax = std::max(cur.pmax, pw_[size_t(k)]); ++cur.width;
            } else if (inG && ++gapRun > 1) {
                inG = false;
                if (ng < 16) groups[ng++] = cur;
            }
        }
        if (inG && ng < 16) groups[ng++] = cur;
        /* the floor: a CENSORED MEAN per bin — one FFT bin of noise is exponentially distributed, so a minimum-tracker
         *  settles at a fraction of the mean and calls 1 bin in 10 a signal. The mean of everything under 3× the floor
         *  (≈ 0.85 of the true mean), with whatever is over it — a signal — barely moving it. */
        for (int k = 0; k < kN; ++k) {
            float& fl = floor_[size_t(k)];
            const float p = pw_[size_t(k)];
            const float a = p < fl * 3.0f ? 0.01f : 0.0003f;
            fl += a * (p - fl);
            if (fl < 1e-14f) fl = 1e-14f;
        }
        if ((frames_ & 255) == 0) {
            std::vector<float> t(floor_.begin(), floor_.end());
            std::nth_element(t.begin(), t.begin() + kN / 2, t.end());
            noiseDb_ = 10.0 * std::log10(std::max(1e-14f, t[size_t(kN / 2)]));
        }
        if (clipFrames_ >= int(w_ / hop_)) { clipFrac_ = double(clipWin_) / clipFrames_; clipWin_ = 0; clipFrames_ = 0; }

        // ★ strongest first, so a strong burst takes its channel before its own splatter can
        std::sort(groups, groups + ng, [](const G& a, const G& b) { return a.pmax > b.pmax; });
        for (auto& c : ch_) if (c.on) c.framePow = 0;
        for (int g = 0; g < ng; ++g) {
            const G& gr = groups[g];
            if (gr.wsum <= 0) continue;
            // ★ a single bin must be clearly strong; a wider group may be weaker — a real burst spreads over bins
            if (gr.width < 2 && gr.peak < thr * 3.0f) continue;
            const double f = gr.f / gr.wsum;
            Chan* hit = nullptr;
            for (auto& c : ch_) if (c.on && std::fabs(c.offHz - f) < kMatchHz) { hit = &c; break; }
            if (hit) { hit->lastHit = total_; ++hit->hits; hit->peakPow = std::max(hit->peakPow, gr.pmax); hit->framePow = std::max(hit->framePow, gr.pmax); continue; }
            /* ★★ SPLATTER IS NOT A BURST. A strong OOK transmitter's keying edges spread −30…−45 dB sidebands hundreds of
             *  kHz either side; each would open a channel of its own. A newcomer 25 dB under a burst that is ON THE AIR
             *  while it is open is taken as its splatter (its keying edges splatter in the frames where it is
             *  switching, not only where it is on). (Two real sensors 25 dB apart at the same instant: the weak
             *  one is lost — the price, and rare.) */
            bool shadow = false;
            for (auto& c : ch_) if (c.on && c.peakPow > gr.pmax * 300.0f) { shadow = true; break; }
            if (shadow) continue;
            Chan* freeC = nullptr;
            for (auto& c : ch_) if (!c.on) { freeC = &c; break; }
            if (!freeC) continue;                              // ★ four bursts at once already: the fifth is lost
            openChan(*freeC, f);
            freeC->peakPow = freeC->framePow = gr.pmax;
        }
        // close channels that went quiet, or ran too long
        for (auto& c : ch_) {
            if (!c.on) continue;
            const double quiet = double(total_ - c.lastHit) / w_;
            const double len = double(total_ - c.start) / w_;
            if (quiet > kCloseS || len > kMaxBurstS) { runChan(c, total_); closeChan(c, onBurst); }
        }
        (void)now;
    }

    void openChan(Chan& c, double f) {
        c.on = true;
        c.offHz = f;
        const uint64_t pre = uint64_t(kPrerollS * w_) + kN;
        c.start = total_ > pre ? total_ - pre : 0;
        c.next = c.start;
        c.lastHit = total_;
        c.hits = 1;
        c.peakPow = 0; c.framePow = 0;
        const double ph = -2.0 * M_PI * f / w_;
        c.inc = cf(float(std::cos(ph)), float(std::sin(ph)));
        c.rot = cf(1, 0);
        c.mcnt = 0; c.renorm = 0; c.acc = 0;
        c.peakAbs = 0;
        c.bb.clear();
        c.bb.reserve(size_t(0.1 * chanRate()));
        runChan(c, total_);
    }

    template <class F> void closeChan(Chan& c, F& onBurst) {
        c.on = false;
        Burst b;
        b.tSec = double(c.start) / w_;
        b.offHz = c.offHz;
        b.clipped = c.peakAbs > 0.85f;
        // ★ fewer than 3 detector frames = a noise spike (1 frame in ~0.3 ms; the shortest real burst spans dozens)
        if (c.hits >= 3 && slice(c.bb, chanRate(), b)) { ++bursts_; onBurst(static_cast<const Burst&>(b)); }
        c.bb.clear();
        c.bb.shrink_to_fit();
    }

public:
    /** A centred moving average of `w` taps (w odd, ≥ 1). */
    static void boxcar(const std::vector<float>& x, size_t w, std::vector<float>& y) {
        const size_t n = x.size(), h = w / 2;
        y.assign(n, 0.0f);
        double acc = 0; size_t lo = 0, hi = 0;   // window [lo, hi)
        for (size_t i = 0; i < n; ++i) {
            const size_t a = i >= h ? i - h : 0, e = std::min(n, i + h + 1);
            while (hi < e) acc += x[hi++];
            while (lo < a) acc -= x[lo++];
            y[i] = float(acc / double(hi - lo));
        }
    }
    /** The shortest-run cluster of a run list, µs (0 if too few runs). */
    static double shortestRun(const Runs& r) {
        if (r.size() < 12) return 0;
        std::vector<int> a; a.reserve(r.size());
        for (int d : r) a.push_back(std::abs(d));
        std::sort(a.begin(), a.end());
        return a[a.size() / 8];
    }

    /** ★ The slicer, public for the tests: baseband at `rate` → b.ook / b.fsk. False = nothing worth decoding.
     *  ★★ TWO PASSES. The channel is ~250 kHz wide because a burst may be anywhere in it and as fast as 17 kbps, but an
     *     Oregon sensor's shortest pulse is ~350 µs: slicing it at 4 µs resolution throws ~15 dB of SNR away. Pass 1
     *     slices lightly smoothed; the burst's own shortest run then sets a smoothing of a fifth of it, and pass 2 slices
     *     again — each burst is filtered to its own speed (a matched filter in all but name). */
    static bool slice(const std::vector<cf>& bb, double rate, Burst& b) {
        const size_t n = bb.size();
        if (n < 64) return false;
        const double us = 1e6 / rate;
        b.sampleUs = float(us);
        std::vector<float> env(n);
        for (size_t i = 0; i < n; ++i) env[i] = std::abs(bb[i]);
        std::vector<float> sm;
        std::vector<uint8_t> on(n), on0;
        b.ookAll.clear(); b.fskAll.clear();
        static const size_t kWins[4] = {3, 9, 25, 61};    // ×4 µs at 250 kS/s: 12, 36, 100, 244 µs
        // the level before the burst (the pre-roll's first ~2 ms) and the burst's own level, from the 9-tap envelope
        {
            boxcar(env, 9, sm);
            const size_t preN = std::min(n / 4, size_t(rate * 0.002));
            std::vector<float> t(sm.begin(), sm.begin() + std::max<size_t>(preN, 8));
            std::sort(t.begin(), t.end());
            float noise = t[t.size() / 2];
            std::vector<float> all(sm);
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.95), all.end());
            const float sig = all[size_t(n * 0.95)];
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.10), all.end());
            noise = std::min(noise, std::max(all[size_t(n * 0.10)], 1e-9f));
            b.levelDb = 20.0f * std::log10(std::max(sig, 1e-9f));
            b.noiseDb = 20.0f * std::log10(std::max(noise, 1e-9f));
        }
        for (size_t wi = 0; wi < 4; ++wi) {
            const size_t win = kWins[wi];
            if (win * us > 0.5 * 1e6 * double(n) / rate / 8) break;   // a window longer than the burst's eighth says nothing
            boxcar(env, win, sm);
            const size_t preN = std::min(n / 4, size_t(rate * 0.002));
            std::vector<float> t(sm.begin(), sm.begin() + std::max<size_t>(preN, 8));
            std::sort(t.begin(), t.end());
            float noise = t[t.size() / 2];
            std::vector<float> all(sm);
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.95), all.end());
            const float sig = all[size_t(n * 0.95)];
            std::nth_element(all.begin(), all.begin() + size_t(n * 0.10), all.end());
            noise = std::min(noise, std::max(all[size_t(n * 0.10)], 1e-9f));
            if (noise <= 0) noise = 1e-9f;
            if (sig < noise * 1.6f) continue;              // under 4 dB at this smoothing: nothing a slicer can use
            const float thr = std::sqrt(sig * noise);     // the midpoint in dB
            const float hi = thr * 1.1f, lo = thr / 1.1f;
            bool st = false;
            for (size_t i = 0; i < n; ++i) { if (!st && sm[i] > hi) st = true; else if (st && sm[i] < lo) st = false; on[i] = st; }
            Runs r = runsOf(on, us, false, int(win / 2) + 2);
            if (r.empty()) continue;
            if (on0.empty()) on0 = on;                      // the finest usable slice marks where the carrier is
            b.ookAll.push_back(std::move(r));
        }
        if (b.ookAll.empty()) return false;
        b.ook = b.ookAll.front();
        on = on0;
        // ── FSK: the discriminator, over the stretch where the carrier is up
        size_t first = n, last = 0, cnt = 0;
        for (size_t i = 0; i < n; ++i) if (on[i]) { if (first == n) first = i; last = i; ++cnt; }
        b.fsk.clear(); b.fskDevHz = 0;
        if (first < last) {
            // the carrier: the mean instantaneous frequency where it is up (refines the detector's bin centroid)
            double acc = 0; size_t na = 0;
            for (size_t i = first + 1; i <= last; ++i) if (on[i] && on[i - 1]) { acc += std::arg(bb[i] * std::conj(bb[i - 1])); ++na; }
            if (na) b.carrierHz = float(acc / double(na) * rate / (2 * M_PI));
        }
        if (first < last && last - first > 32 && double(cnt) / double(last - first + 1) > 0.85) {
            const size_t m = last - first + 1;
            std::vector<float> fq(m);
            const float k = float(rate / (2 * M_PI));
            for (size_t i = 0; i < m; ++i) {
                const size_t j = first + i;
                fq[i] = j ? std::arg(bb[j] * std::conj(bb[j - 1])) * k : 0.0f;
            }
            std::vector<float> fs;
            static const size_t kFw[3] = {3, 9, 25};
            for (size_t wi = 0; wi < 3; ++wi) {
                boxcar(fq, kFw[wi], fs);
                std::vector<float> s2(fs);
                std::nth_element(s2.begin(), s2.begin() + size_t(m * 0.1), s2.end());
                const float p10 = s2[size_t(m * 0.1)];
                std::nth_element(s2.begin(), s2.begin() + size_t(m * 0.9), s2.end());
                const float p90 = s2[size_t(m * 0.9)];
                const float dev = 0.5f * (p90 - p10), mid = 0.5f * (p90 + p10);
                if (dev <= 4000.0f) continue;
                if (b.fskAll.empty()) { b.fskDevHz = dev; b.carrierHz = mid; }
                std::vector<uint8_t> hiTone(m);
                bool sv = fs[0] > mid;
                const float hy = dev * 0.15f;
                for (size_t i = 0; i < m; ++i) {
                    if (!sv && fs[i] > mid + hy) sv = true; else if (sv && fs[i] < mid - hy) sv = false;
                    hiTone[i] = sv;
                }
                b.fskAll.push_back(runsOf(hiTone, us, /*leadingSpaceOk=*/true, int(kFw[wi] / 2) + 2));
            }
            if (!b.fskAll.empty()) b.fsk = b.fskAll.front();
        }
        return !b.ook.empty();
    }

    /** Level runs → signed µs runs, with one- and two-sample glitches folded into their neighbours. */
    static Runs runsOf(const std::vector<uint8_t>& lv, double us, bool leadingSpaceOk = false, int glitch = 2) {
        std::vector<std::pair<uint8_t, int>> r;
        for (size_t i = 0; i < lv.size(); ++i) {
            if (r.empty() || r.back().first != lv[i]) r.push_back({lv[i], 1});
            else ++r.back().second;
        }
        // fold glitches (≤ `glitch` samples) that sit between two runs of the other level
        std::vector<std::pair<uint8_t, int>> g;
        for (size_t i = 0; i < r.size(); ++i) {
            if (r[i].second <= glitch && !g.empty() && i + 1 < r.size()) {
                g.back().second += r[i].second + r[i + 1].second;
                ++i;
                continue;
            }
            if (!g.empty() && g.back().first == r[i].first) g.back().second += r[i].second;
            else g.push_back(r[i]);
        }
        Runs out;
        bool started = leadingSpaceOk;
        for (auto& p : g) {
            if (!started && !p.first) continue;            // OOK: the runs start at the first mark
            started = true;
            const int d = int(std::lround(p.second * us));
            out.push_back(p.first ? d : -d);
        }
        return out;
    }

private:
    int pre_ = 1, preCnt_ = 0, chanM_ = 1, hop_ = kN, sinceFft_ = 0;
    cf preAcc_{0, 0};
    double w_ = 0, dcGuardHz_ = 0, noiseDb_ = -99, clipFrac_ = 0;
    int clipWin_ = 0, clipFrames_ = 0;
    float blockPeak_ = 0;
    std::vector<cf> ring_;
    uint64_t total_ = 0, frames_ = 0, bursts_ = 0;
    SmallFft fft_;
    std::vector<float> win_, floor_, pw_;
    std::vector<cf> fbuf_;
    Chan ch_[kMaxChans];
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SLICERS — runs → bits. Every protocol decoder is built from these.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
using Bits = std::vector<uint8_t>;   ///< one bit per element, in order of transmission

inline bool near(int d, double want, double tol) { return std::fabs(std::abs(d) - want) <= want * tol; }

/** PWM: the MARK width carries the bit; a space longer than `rowGapUs` ends a row. `shortIsOne` as the chip says. */
inline std::vector<Bits> slicePwm(const Runs& r, double shortUs, double longUs, double rowGapUs, bool shortIsOne,
                                  double tol = 0.4) {
    std::vector<Bits> rows(1);
    for (size_t i = 0; i < r.size(); ++i) {
        const int d = r[i];
        if (d > 0) {
            const double ds = std::fabs(d - shortUs), dl = std::fabs(d - longUs);
            const bool isShort = ds < dl;
            if ((isShort && ds > shortUs * tol) || (!isShort && dl > longUs * tol)) { if (!rows.back().empty()) rows.emplace_back(); continue; }
            rows.back().push_back(uint8_t(isShort == shortIsOne));
        } else if (-d > rowGapUs && !rows.back().empty()) rows.emplace_back();
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** PPM: the SPACE after each mark carries the bit (short = 0, long = 1 unless `shortIsOne`); a longer space ends a row.
 *  The last mark of a row has no space to read and carries nothing. */
inline std::vector<Bits> slicePpm(const Runs& r, double shortGapUs, double longGapUs, double rowGapUs, bool shortIsOne = false,
                                  double tol = 0.35) {
    std::vector<Bits> rows(1);
    for (size_t i = 0; i < r.size(); ++i) {
        const int d = r[i];
        if (d > 0) continue;
        const double g = -d;
        if (g > rowGapUs) { if (!rows.back().empty()) rows.emplace_back(); continue; }
        const double ds = std::fabs(g - shortGapUs), dl = std::fabs(g - longGapUs);
        const bool isShort = ds < dl;
        if ((isShort && ds > shortGapUs * tol) || (!isShort && dl > longGapUs * tol)) { if (!rows.back().empty()) rows.emplace_back(); continue; }
        rows.back().push_back(uint8_t(isShort == shortIsOne));
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** NRZ: each run is round(d / bitUs) bits of its level (mark = 1). A run over `maxBits` ends a row. */
inline std::vector<Bits> sliceNrz(const Runs& r, double bitUs, int maxBits = 24) {
    std::vector<Bits> rows(1);
    for (int d : r) {
        const int nb = int(std::lround(std::abs(d) / bitUs));
        if (nb > maxBits || nb == 0) { if (!rows.back().empty()) rows.emplace_back(); continue; }
        for (int k = 0; k < nb; ++k) rows.back().push_back(d > 0 ? 1 : 0);
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** Runs → HALF-BIT levels for Manchester: each run is 1 or 2 half-bits (a short or a long); anything else ends a row.
 *  Separate short/long limits for marks and spaces, because many transmitters trim their pulses (Oregon: ~100 µs). */
struct HalfLimits { double markShortMax, markLongMax, spaceShortMax, spaceLongMax, markMin, spaceMin; };
inline HalfLimits halfLimits(double halfUs, double markTrimUs = 0) {
    return { 1.5 * halfUs - markTrimUs, 2.5 * halfUs - markTrimUs, 1.5 * halfUs + markTrimUs, 2.5 * halfUs + markTrimUs,
             0.4 * halfUs - markTrimUs * 0.5, 0.4 * halfUs };
}
inline std::vector<Bits> halfBits(const Runs& r, const HalfLimits& L) {
    std::vector<Bits> rows(1);
    for (int d : r) {
        const bool m = d > 0; const double a = std::abs(d);
        int k = 0;
        if (m) k = a < L.markMin ? -1 : a <= L.markShortMax ? 1 : a <= L.markLongMax ? 2 : 0;
        else   k = a < L.spaceMin ? -1 : a <= L.spaceShortMax ? 1 : a <= L.spaceLongMax ? 2 : 0;
        if (k <= 0) {
            // ★ a long run ends the row, but its first half-bit still belongs to it (the last bit of a frame is
            //   followed by silence: mark-then-nothing must still read as the bit's first half)
            if (k == 0 && !rows.back().empty()) { rows.back().push_back(m ? 1 : 0); rows.emplace_back(); }
            else if (!rows.back().empty()) rows.emplace_back();
            continue;
        }
        for (int i = 0; i < k; ++i) rows.back().push_back(m ? 1 : 0);
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}
/** Half-bit levels → bits, pairs from `phase` (0/1). `hiFirstIsOne`: (1,0) = 1 (IEEE 802.3 calls that 0 — conventions
 *  differ by vendor; each decoder says which). Stops at the first violation (a pair of equal halves). */
inline Bits manchester(const Bits& h, int phase, bool hiFirstIsOne) {
    Bits out;
    for (size_t i = size_t(phase); i + 1 < h.size(); i += 2) {
        if (h[i] == h[i + 1]) break;
        out.push_back(uint8_t((h[i] == 1) == hiFirstIsOne));
    }
    return out;
}
/** Differential Manchester: a transition in the middle of every bit; bit = whether the level CHANGED at the bit's start
 *  (`changeIsZero`: a change at the boundary = 0, the common convention). */
inline Bits diffManchester(const Bits& h, int phase, bool changeIsZero = true) {
    Bits out;
    int prevSecond = -1;
    for (size_t i = size_t(phase); i + 1 < h.size(); i += 2) {
        if (h[i] == h[i + 1]) break;
        if (prevSecond >= 0) {
            const bool changed = h[i] != prevSecond;
            out.push_back(uint8_t(changed != changeIsZero));
        }
        prevSecond = h[i + 1];
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  BIT HELPERS + CHECKS
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint32_t bitsAt(const Bits& b, size_t pos, int n) {   ///< MSB-first
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 1) | (pos + size_t(i) < b.size() ? b[pos + size_t(i)] : 0);
    return v;
}
inline uint32_t bitsAtLsb(const Bits& b, size_t pos, int n) {   ///< LSB-first
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) if (pos + size_t(i) < b.size() && b[pos + size_t(i)]) v |= 1u << i;
    return v;
}
inline std::vector<uint8_t> bytesAt(const Bits& b, size_t pos, int nBytes) {
    std::vector<uint8_t> o(static_cast<size_t>(nBytes));
    for (int i = 0; i < nBytes; ++i) o[size_t(i)] = uint8_t(bitsAt(b, pos + size_t(i) * 8, 8));
    return o;
}
/** Find `pattern` (`n` bits, MSB-first) in `b` from `from`; npos if absent. */
inline size_t findBits(const Bits& b, uint32_t pattern, int n, size_t from = 0) {
    if (b.size() < size_t(n)) return size_t(-1);
    for (size_t i = from; i + size_t(n) <= b.size(); ++i) if (bitsAt(b, i, n) == pattern) return i;
    return size_t(-1);
}
inline Bits invert(Bits b) { for (auto& x : b) x ^= 1; return b; }
/** CRC-8, MSB-first, no reflection (the "CRC-8" of most sensor write-ups: poly 0x31 Dallas/Maxim-style, 0x07 CCITT). */
inline uint8_t crc8(const uint8_t* d, size_t n, uint8_t poly, uint8_t init = 0) {
    uint8_t c = init;
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = uint8_t(c & 0x80 ? (c << 1) ^ poly : c << 1); }
    return c;
}
/** CRC-16, MSB-first, no reflection (CRC-16-CCITT = poly 0x1021). */
inline uint16_t crc16(const uint8_t* d, size_t n, uint16_t poly, uint16_t init) {
    uint16_t c = init;
    for (size_t i = 0; i < n; ++i) { c ^= uint16_t(d[i]) << 8; for (int k = 0; k < 8; ++k) c = uint16_t(c & 0x8000 ? (c << 1) ^ poly : c << 1); }
    return c;
}

} // namespace vibeism
