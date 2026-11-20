// vibe_varatag.h — the VARA HF IDENTIFICATION TAG (V12, 2026-11-19; docs/v12/VARA-TAG.md). IDENTIFY ONLY.
//
// ★★★ LEGAL — Stuart, 2026-11-19: VARA must NEVER be decoded in any form. Its author sells the modem and enforces that
//     with cease-and-desist letters; an open-source VARA receiver had to shut down. So this file:
//      • reads ONLY outward, publicly documented signal measurements — the occupied bandwidth (VARA HF's published
//        2750 / 2300 / 500 Hz modes), the regular comb of carriers in the averaged POWER spectrum and its spacing, and
//        how the block comes and goes in time (ARQ: long bursts answered by short ones near the same centre);
//      • never demodulates a carrier, never looks at a phase, a symbol, a frame, a scrambler or a code — it has no
//        notion of any of them. It is a spectrum-shape classifier, the same kind of measurement as the lightning badge;
//      • says only "VARA HF (proprietary — not decoded)". The words live in the clients (knownSignals.ts VARA_TAG_LABEL
//        and the R36S copy); the server sends the kind "vara" and where.
//     Nothing here may ever grow into a VARA decoder or receiver. If a future change needs anything beyond the power
//     spectrum's shape, the answer is no.
//
// ★★ HOW (two stages, so a quiet band costs almost nothing):
//   1. ROW (stage 1) — the averaged wide spectrum row the server already makes for the waterfall (LocalSdrShim::
//      onSpectrum, ~47–75 Hz bins; NO new transform). At ≤ 4 Hz: a local noise floor, runs of bins above it, and for each
//      run its −6 dB width, flatness and edges. A flat, steep-sided block whose width is one of VARA's (±10 %) becomes a
//      TRACK; its on/off history over the last 90 s is the burst pattern.
//   2. COMB (stage 2) — the bins are too coarse to see carriers ~47 Hz apart, so ONE candidate at a time (K = 1) gets a
//      narrow complex slice of the SAME forward FFT every listener's channel is cut from (one small inverse transform a
//      block — the shim's chan_->extract, as a CW skimmer's slice). Its averaged power spectrum at ~2.5 Hz resolution,
//      taken only while the block is up, has the block's slow shape removed; what is left is the ripple of the carriers.
//      A periodogram OF THAT RIPPLE names its period: the carrier spacing. VARA's (measured on real signals, see
//      kSpacingLo/Hi) must be the strongest period between 20 and 200 Hz, and strong enough.
//   A track is TAGGED when its shape and comb agree (confidence ≥ kTagConf) and it has persisted or burst; it stays
//   tagged through ARQ pauses for kHoldS and goes when the confidence drops below kDropConf.
// ★★ LOOK-ALIKES, and what keeps each one out (test-varatag holds every one to 0 labels):
//   PACTOR III — tones 120 Hz apart: the comb's period is 120, not ~47.  PACTOR IV, STANAG 4285, MIL-STD-188-110 — one
//   carrier: no comb at all.  FreeDV 700D/700E and RADE V1 — OFDM with similar spacing (RADE 50 Hz) but 1–1.5 kHz wide:
//   no width class, and RADE's 50 Hz falls outside the spacing window too.  DRM — 4.5 kHz and up: no width class.
//   Olivia / Contestia / MFSK / MT63 — tone spacings 7.8–125 Hz (31.25 and 62.5 most of all): the period is wrong.
//   SSB speech — neither flat nor steep-edged, and no fixed comb.  Noise — no block.
// ★ COST: measured in test-varatag (stage 1 per row, stage 2 per second of slice) — see docs/v12/VARA-TAG.md.
// ★ Thread: everything is called on the DSP thread (onSpectrum and the channeliser's block callback), like the lightning
//   detector; json() may be read from any thread (its own mutex).
#pragma once
#include "vibedsp/vibedsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vibe {
namespace varatag {

using cf32 = vibedsp::cf32;

/** VARA HF's published occupied bandwidths (sigidwiki "VARA HF": 2750 Tactical, 2300 Standard, 500 Narrow). */
constexpr double kClassHz[3] = { 500.0, 2300.0, 2750.0 };
/** ★★ THE CARRIER SPACING WINDOW (Hz). The published figures imply ~46.9 Hz (2300 Hz / 49 carriers, 2750 / 59); the
 *  window is what real VARA measured on Stuart's UberSDR (docs/v12/VARA-TAG.md "Real signals"), with room for the
 *  measurement's own ±0.7 Hz. RADE V1's 50 Hz and Olivia/MT63's 31.25 / 62.5 sit well outside it. */
constexpr double kSpacingLo = 45.3, kSpacingHi = 48.5;
/** Stage 1 runs at most this often (s) — the waterfall's own rate is far higher than a shape needs. */
constexpr double kRowPeriodS = 0.25;
/** A bin this far above the local floor (dB) is "signal". */
constexpr float kSnrDb = 7.0f;
/** Comb evaluation: segments wanted, the fewest accepted, and the longest it may take (s). */
constexpr int kSegsWant = 40, kSegsMin = 18;
/** ★ An inconclusive look — the strongest period IS in the VARA window but still faint — keeps averaging, doubling the
 *  segments up to this many (more averaging only ever sharpens a real comb; noise and look-alikes stay where they are). */
constexpr int kSegsMax = 160;
constexpr double kEvalMaxS = 80.0, kEvalMaxLongS = 240.0;
/** A track's comb result is re-checked after this (s). */
constexpr double kCombFreshS = 240.0;
/** The comb's periodogram value (0…0.5 for a pure ripple) that counts as a clear comb, and where it scores 1. */
constexpr float kCombMin = 0.10f, kCombFull = 0.15f;
/** Confidence: tag at or above, untag below; a tag held this long after its block was last seen (ARQ pauses). */
constexpr float kTagConf = 0.85f, kDropConf = 0.60f;
constexpr double kHoldS = 60.0;
/** A track is forgotten this long after it was last seen. */
constexpr double kForgetS = 120.0;
constexpr int kMaxTracks = 24;
/** Bins of dip bridged inside one block (a fade's notch), at the wide row's 47–75 Hz bins ≈ 190–300 Hz. */
constexpr int kBridge = 4;
/** Confidence = kShapeW × shape + (1 − kShapeW) × comb: the comb is the evidence, the shape only the candidate. */
constexpr float kShapeW = 0.15f;

struct Tag { int id; double hz; double bw; float conf; double spacing; };

/** ★ The comb's slice size, ONE rule for the shim and test-varatag: the smallest power-of-two count of forward-FFT bins
 *  (Channelizer::extract's chanBins) whose rate holds the widest class twice over plus a guard — the guard bins beside
 *  the block are the segment gate's reference. */
inline int sliceBins(double sampleRate, int fftSize, double widthHz) {
    const double want = std::max(1500.0, 2.0 * widthHz + 600.0);
    int b = 8;
    while (b < fftSize / 4 && sampleRate * b / fftSize < want) b *= 2;
    return b;
}

/** Which VARA width class a measured width belongs to (−1 = none): 380–660 Hz narrow, 2050–2600, 2600–3100 — the
 *  measured width runs ~100–250 Hz over the true one at the wide row's 47–75 Hz unwindowed bins (leakage), so the split
 *  between 2300 and 2750 sits at 2600, not halfway. */
inline int widthClass(double w) {
    if (w >= 380.0 && w <= 660.0) return 0;
    if (w >= 2050.0 && w < 2600.0) return 1;
    if (w >= 2600.0 && w <= 3100.0) return 2;
    return -1;
}

/**
 * ★ The comb measurement, on its own so test-varatag can hold it to signals directly: `db` = an averaged power
 * spectrum in dB, `n` bins `binHz` apart, covering only the block's flat top. Returns the periodogram's strongest
 * period in [20, 200] Hz (`bestHz`) and its value (`bestP`), and the strongest inside the VARA window (`varaP`).
 */
struct CombResult { double bestHz = 0; float bestP = 0; float varaP = 0; double varaHz = 0; };
inline CombResult measureComb(const float* db, int n, double binHz) {
    CombResult r;
    if (n < 32 || !(binHz > 0)) return r;
    // ① the slow shape out: a moving average over ~150 Hz (prefix sums)
    std::vector<double> pre((size_t)n + 1, 0.0);
    for (int i = 0; i < n; i++) pre[(size_t)i + 1] = pre[(size_t)i] + db[i];
    const int h = std::max(2, (int)std::lround(75.0 / binHz));
    std::vector<float> d((size_t)n);
    double e2 = 0;
    for (int i = 0; i < n; i++) {
        const int a = std::max(0, i - h), b = std::min(n, i + h + 1);
        float v = (float)(db[i] - (pre[(size_t)b] - pre[(size_t)a]) / (double)(b - a));
        // ② a narrow carrier or a fade's notch must not BE the ripple: clipped to ±6 dB
        v = std::max(-6.0f, std::min(6.0f, v));
        d[(size_t)i] = v; e2 += (double)v * v;
    }
    if (!(e2 > 1e-9)) return r;
    // ③ the ripple's periodogram over period s ∈ [20, 200] Hz, stepped evenly in 1/s (an eighth of its resolution)
    const double bw = n * binHz;
    const double qLo = 1.0 / 200.0, qHi = 1.0 / 20.0, dq = 1.0 / (8.0 * bw);
    for (double q = qLo; q <= qHi; q += dq) {
        double re = 0, im = 0;
        const double w = 2.0 * M_PI * q * binHz;
        // recursive rotator: one complex multiply a bin
        double cr = 1, ci = 0; const double sr = std::cos(w), si = -std::sin(w);
        for (int i = 0; i < n; i++) {
            re += d[(size_t)i] * cr; im += d[(size_t)i] * ci;
            const double t = cr * sr - ci * si; ci = cr * si + ci * sr; cr = t;
        }
        const float p = (float)((re * re + im * im) / ((double)n * e2));
        const double s = 1.0 / q;
        if (p > r.bestP) { r.bestP = p; r.bestHz = s; }
        if (s >= kSpacingLo && s <= kSpacingHi && p > r.varaP) { r.varaP = p; r.varaHz = s; }
    }
    return r;
}

class Tagger {
public:
    Tagger() = default;

    /** Forget everything (a retune of the capture, the owner's switch off). */
    void reset() {
        tracks_.clear(); active_ = -1; activeSeg_ = 0;
        std::lock_guard<std::mutex> lk(mtx_);
        tags_.clear(); json_ = kEmpty;
    }

    /**
     * Stage 1: one averaged wide row. `sum` × `scale` = dB per bin, fftshifted (bin `bins/2` = `centreHz`), `binHz` apart.
     * The caller has already gated HF (the capture below 30 MHz) and the owner's switch.
     */
    void row(const float* sum, float scale, int bins, double centreHz, double binHz, double nowS) {
        if (bins < 64 || !(binHz > 0) || binHz > 100.0) return;
        if (nowS - lastRowS_ < kRowPeriodS) return;
        lastRowS_ = nowS;
        if (centreHz != rowCentre_ || binHz != rowBinHz_) {
            // ★ the capture moved: tracks are kept in RF, but a big move makes them stale — forget those out of view
            rowCentre_ = centreHz; rowBinHz_ = binHz;
        }
        rows_++;
        // ① the local floor: block medians, then a low percentile of the blocks around each one
        const int B = std::max(6, (int)std::lround(700.0 / binHz));
        const int nb = bins / B;
        if (nb < 8) return;
        blk_.resize((size_t)nb);
        tmp_.resize((size_t)B);
        for (int k = 0; k < nb; k++) {
            for (int j = 0; j < B; j++) tmp_[(size_t)j] = sum[k * B + j] * scale;
            std::nth_element(tmp_.begin(), tmp_.begin() + B / 2, tmp_.end());
            blk_[(size_t)k] = tmp_[(size_t)B / 2];
        }
        floor_.resize((size_t)nb);
        const int W = 12;
        std::vector<float>& win = tmp2_;
        for (int k = 0; k < nb; k++) {
            win.clear();
            for (int j = std::max(0, k - W); j <= std::min(nb - 1, k + W); j++) win.push_back(blk_[(size_t)j]);
            const size_t q = win.size() * 3 / 10;
            std::nth_element(win.begin(), win.begin() + q, win.end());
            floor_[(size_t)k] = win[q];
        }
        auto lev = [&](int i) { return sum[i] * scale; };
        auto flo = [&](int i) { const int k = std::min(nb - 1, i / B); return floor_[(size_t)k]; };
        // ② runs above floor + kSnrDb (a one-bin dip bridged), the outer 3 % of the capture skipped (anti-alias roll-off)
        seen_.clear();
        const int i0 = bins * 3 / 100, i1 = bins - bins * 3 / 100;
        int i = i0;
        while (i < i1) {
            if (lev(i) < flo(i) + kSnrDb) { i++; continue; }
            int j = i + 1;
            // ★ a fade's notch (two-path HF: −10 dB dips every ~1 kHz) must not cut one block into two: dips of up to
            //   kBridge bins are bridged
            auto up = [&](int q) { return q < i1 && lev(q) >= flo(q) + kSnrDb; };
            while (j < i1) { bool any = false; for (int q = 0; q <= kBridge; q++) if (up(j + q)) { any = true; break; } if (!any) break; j++; }
            measureRun(sum, scale, bins, i, j, centreHz, binHz, flo);
            i = j + 1;
        }
        updateTracks(nowS);
    }

    /** Stage 2's request: the candidate the comb wants a slice of (centre, width Hz); false = none (zero cost). */
    bool wantSlice(double& centreHz, double& widthHz) const {
        if (active_ < 0) return false;
        const Track* t = find(active_);
        if (!t) return false;
        centreHz = t->hz; widthHz = kClassHz[t->cls];
        return true;
    }

    /** Stage 2: `n` samples of the requested slice at `rate`, its centre at `centreRfHz`. */
    void slice(const cf32* x, int n, double rate, double centreRfHz, double nowS) {
        Track* t = find(active_);
        if (!t || n <= 0 || !(rate > 0)) { active_ = -1; return; }
        if (rate != segRate_) {
            segRate_ = rate;
            M_ = 256; while (M_ < 8192 && rate / M_ > 2.6) M_ *= 2;
            fft_.reset(new vibedsp::ComplexFFT(M_));
            win_.resize((size_t)M_);
            for (int k = 0; k < M_; k++) win_[(size_t)k] = (float)(0.5 - 0.5 * std::cos(2.0 * M_PI * k / M_));
            ring_.clear(); psd_.assign((size_t)M_, 0.0); activeSeg_ = 0;
            buf_.resize((size_t)M_); out_.resize((size_t)M_); pw_.resize((size_t)M_);
        }
        sliceCentre_ = centreRfHz;
        for (int k = 0; k < n; k++) {
            ring_.push_back(x[k]);
            if ((int)ring_.size() >= M_) {
                segment(*t);
                ring_.erase(ring_.begin(), ring_.begin() + M_ / 2);   // ½ overlap
            }
        }
        sliceSamples_ += n;
        if (activeSeg_ >= segTarget_ || nowS - evalStartS_ > evalMaxS_) finishEval(*t, nowS);
    }

    /** The tags now, as the wire's `sigtags` message (kind "vara"). Any thread. */
    std::string json() const { std::lock_guard<std::mutex> lk(mtx_); return json_; }
    std::vector<Tag> tags() const { std::lock_guard<std::mutex> lk(mtx_); return tags_; }
    bool any() const { std::lock_guard<std::mutex> lk(mtx_); return !tags_.empty(); }

    // ── for the tests and the setup page's cost line ──
    struct Stats { long long rows = 0, segs = 0, evals = 0, evalsPass = 0; long long sliceSamples = 0;
                   long long rejWidth = 0, rejSteep = 0, rejFlat = 0, rejFill = 0, rejSnr = 0; };
    Stats stats() const { return { rows_, segsTotal_, evals_, evalsPass_, sliceSamples_, rej_[0], rej_[1], rej_[2], rej_[3], rej_[4] }; }
    /** The last comb measurement (tests print it). */
    CombResult lastComb() const { return lastComb_; }
    double lastWidth() const { return lastWidth_; }

    static constexpr const char* kEmpty = "{\"type\":\"sigtags\",\"v\":1,\"tags\":[]}";

private:
    struct Run { double hz, w; int cls; float shape; };
    struct Track {
        int id; double hz; int cls; double firstS, lastOnS; bool on = false; double onSinceS = 0;
        std::deque<std::pair<double, double>> runs;   // on intervals (start, end), the last 90 s
        float shape = 0; float comb = 0; double combAt = -1e18; double spacing = 0;
        bool tagged = false; float conf = 0; bool hit = false;
        int votes[3] = { 0, 0, 0 };   // readings per width class
        double w = 0;                 // the measured width (Hz), smoothed
    };

    template <class Flo>
    void measureRun(const float* sum, float scale, int bins, int a, int b, double centreHz, double binHz, Flo flo) {
        if (b - a < 3) return;
        // the run's top: its median level
        tmp_.clear();
        for (int k = a; k < b; k++) tmp_.push_back(sum[k] * scale);
        std::nth_element(tmp_.begin(), tmp_.begin() + tmp_.size() / 2, tmp_.end());
        const float top = tmp_[tmp_.size() / 2];
        // ★ the width: from the run's middle outwards to where the level falls 10 dB under the top (or to the floor's
        //   threshold on a weak block), dips of up to kBridge bins bridged — a fade's notch inside the block never ends it
        const int mid = (a + b) / 2;
        const float fl = flo(mid);
        const float L = std::max(fl + kSnrDb, top - 10.0f);
        auto above = [&](int k) { return k >= 0 && k < bins && sum[k] * scale >= L; };
        auto aboveAny = [&](int k, int dir) { for (int q = 0; q < kBridge; q++) if (above(k + dir * q)) return true; return false; };
        int lo = mid, hi = mid;
        while (lo - 1 >= 0 && aboveAny(lo - 1, -1)) lo--;
        while (hi + 1 < bins && aboveAny(hi + 1, +1)) hi++;
        while (lo < hi && !above(lo)) lo++;
        while (hi > lo && !above(hi)) hi--;
        const double w = (hi - lo + 1) * binHz;
        lastWidth_ = w;
        const int cls = widthClass(w);
        if (cls < 0) { if (w > 300 && w < 3500) rej_[0]++; return; }
        const int nIn = hi - lo + 1;
        // ★ flat top: the inner 80 % within a few dB (std dev), and most of it within 6 dB of the top
        const int ia = lo + nIn / 10, ib = hi - nIn / 10;
        double m = 0, s2 = 0; int cnt = 0, close = 0;
        for (int k = ia; k <= ib; k++) { const double v = sum[k] * scale; m += v; s2 += v * v; cnt++; if (v >= top - 6.0f) close++; }
        if (cnt < 3) return;
        m /= cnt; const double sd = std::sqrt(std::max(0.0, s2 / cnt - m * m));
        const double fill = (double)close / cnt;
        // ★ steep sides: three and four bins beyond each edge are back at the floor (an OFDM block's edge is a bin or two
        //   wide even unwindowed; SSB speech's roll-off spans many)
        auto outside = [&](int k) { return k < 0 || k >= bins ? -300.0f : sum[k] * scale; };
        //   ★ the wide FFT is UNWINDOWED (overlap-save needs it), so a strong block leaks: ~−12 dB three bins out. The limit
        //     is the floor's or 12 dB under the top, whichever is higher.
        const float lim = std::max(fl + 4.5f, top - 12.0f);
        const bool steep = std::max(outside(lo - 3), outside(lo - 4)) <= lim && std::max(outside(hi + 3), outside(hi + 4)) <= lim;
        if (!steep) { rej_[1]++; return; }
        if (sd > 4.5) { rej_[2]++; return; }
        if (fill < 0.70) { rej_[3]++; return; }
        if (top - fl < kSnrDb + 1.0f) { rej_[4]++; return; }
        // ★ 1 = dead flat and full; a faded block (notches) scores lower but still counts — the comb decides
        const float shape = (float)std::max(0.0, std::min(1.0, 1.0 - (sd - 1.5) / 4.0)) * (float)std::min(1.0, fill / 0.9);
        seen_.push_back({ centreHz + ((lo + hi) * 0.5 - bins / 2) * binHz, w, cls, shape });
    }

    Track* find(int id) { for (auto& t : tracks_) if (t.id == id) return &t; return nullptr; }
    const Track* find(int id) const { for (auto& t : tracks_) if (t.id == id) return &t; return nullptr; }

    void updateTracks(double now) {
        for (auto& t : tracks_) t.hit = false;
        for (const Run& r : seen_) {
            Track* best = nullptr; double bd = 1e18;
            // ★ matched by CENTRE, not by class: a 2300 Hz block read at 75 Hz bins straddles 2520 Hz now and then, and
            //   one signal must stay one track (its class is the majority of its readings)
            for (auto& t : tracks_) {
                const double tol = std::max(120.0, 0.12 * std::max(kClassHz[t.cls], kClassHz[r.cls]));
                if ((t.cls == 0) != (r.cls == 0)) continue;   // the narrow class never merges with a wide one
                const double d = std::fabs(t.hz - r.hz);
                if (d <= tol && d < bd) { bd = d; best = &t; }
            }
            if (!best) {
                if ((int)tracks_.size() >= kMaxTracks) {
                    auto old = std::min_element(tracks_.begin(), tracks_.end(), [](const Track& x, const Track& y) {
                        return (x.tagged ? 1e12 : 0) + x.lastOnS < (y.tagged ? 1e12 : 0) + y.lastOnS; });
                    if (old->id == active_) active_ = -1;
                    tracks_.erase(old);
                }
                Track nt; nt.id = nextId_++; nt.hz = r.hz; nt.cls = r.cls; nt.firstS = now; nt.lastOnS = now; nt.shape = r.shape; nt.w = r.w;
                tracks_.push_back(nt);
                best = &tracks_.back();
            }
            best->hit = true;
            if (best->votes[r.cls] < 1000000) best->votes[r.cls]++;
            best->cls = (int)(std::max_element(best->votes, best->votes + 3) - best->votes);
            best->hz += 0.2 * (r.hz - best->hz);
            best->w += 0.2 * (r.w - best->w);
            best->shape += 0.2f * (r.shape - best->shape);
            if (!best->on) { best->on = true; best->onSinceS = now; }
            best->lastOnS = now;
        }
        for (auto& t : tracks_) {
            if (!t.hit && t.on && now - t.lastOnS > 0.6) {
                t.on = false; t.runs.push_back({ t.onSinceS, t.lastOnS });
            }
            while (!t.runs.empty() && t.runs.front().second < now - 90.0) t.runs.pop_front();
        }
        // forget the long gone
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& t) {
            const bool gone = now - t.lastOnS > kForgetS;
            if (gone && t.id == active_) active_ = -1;
            return gone; }), tracks_.end());
        // ★ stage 2's next candidate: on recently, enough on-time, no fresh comb result
        if (active_ < 0) {
            Track* pick = nullptr; double bestOn = 0;
            for (auto& t : tracks_) {
                if (now - t.combAt < kCombFreshS) continue;
                const double on = onTime(t, now);
                if (now - t.lastOnS < 2.0 && on >= 3.0 && on > bestOn) { bestOn = on; pick = &t; }
            }
            if (pick) { active_ = pick->id; evalStartS_ = now; activeSeg_ = 0; segTarget_ = kSegsWant; evalMaxS_ = kEvalMaxS; std::fill(psd_.begin(), psd_.end(), 0.0); ring_.clear(); }
        } else if (Track* t = find(active_)) {
            if (now - evalStartS_ > evalMaxS_) finishEval(*t, now);
        }
        publish(now);
    }

    /** Half the part of the block the comb reads: 80 % of the narrower of its measured width and its class (the edges and
     *  their leakage stay out of the ripple). */
    static double inbandHalf(const Track& t) { return 0.4 * std::min(t.w > 0 ? t.w : kClassHz[t.cls], kClassHz[t.cls]); }

    static double onTime(const Track& t, double now) {
        double s = 0;
        for (const auto& r : t.runs) if (r.second > now - 90.0) s += r.second - std::max(r.first, now - 90.0);
        if (t.on) s += t.lastOnS - std::max(t.onSinceS, now - 90.0);
        return s;
    }
    static int bursts(const Track& t, double now) {
        int b = 0;
        for (const auto& r : t.runs) if (r.second > now - 90.0 && r.second - r.first >= 0.4) b++;
        return b + (t.on ? 1 : 0);
    }

    /** One M-point segment of the slice: kept only while the block is UP (its band well above the slice's guard bins). */
    void segment(Track& t) {
        for (int k = 0; k < M_; k++) buf_[(size_t)k] = ring_[(size_t)k] * win_[(size_t)k];
        fft_->forward(buf_.data(), out_.data());
        const double fb = segRate_ / M_;
        for (int k = 0; k < M_; k++) {           // fftshift: index M_/2 = the slice's centre
            const cf32 c = out_[(size_t)((k + M_ / 2) % M_)];
            pw_[(size_t)k] = c.real() * c.real() + c.imag() * c.imag();
        }
        const double half = inbandHalf(t), off = t.hz - sliceCentre_, edge = std::max(half / 0.8, kClassHz[t.cls] / 2.0);
        const int a = M_ / 2 + (int)std::lround((off - half) / fb), b = M_ / 2 + (int)std::lround((off + half) / fb);
        const int ga = M_ / 2 + (int)std::lround((off + edge + 150.0) / fb), gb = M_ / 2 + (int)std::lround(segRate_ * 0.36 / fb);
        const int la = M_ / 2 - (int)std::lround(segRate_ * 0.36 / fb), lb = M_ / 2 + (int)std::lround((off - edge - 150.0) / fb);
        if (a < 1 || b >= M_ - 1 || b - a < 16) return;
        double in = 0, g = 0; int gn = 0;
        for (int k = a; k <= b; k++) in += pw_[(size_t)k];
        in /= (b - a + 1);
        for (int k = std::max(0, ga); k < std::min(M_, gb); k++) { g += pw_[(size_t)k]; gn++; }
        for (int k = std::max(0, la); k < std::min(M_, lb); k++) { g += pw_[(size_t)k]; gn++; }
        if (gn < 8) return;
        g /= gn;
        if (!(in > g * 6.3)) return;             // ★ +8 dB: the block is up in this segment
        for (int k = 0; k < M_; k++) psd_[(size_t)k] += pw_[(size_t)k];
        activeSeg_++; segsTotal_++;
    }

    void finishEval(Track& t, double now) {
        evals_++;
        t.combAt = now;
        if (activeSeg_ >= kSegsMin) {
            const double fb = segRate_ / M_;
            const double half = inbandHalf(t), off = t.hz - sliceCentre_;
            const int a = M_ / 2 + (int)std::lround((off - half) / fb), b = M_ / 2 + (int)std::lround((off + half) / fb);
            std::vector<float> db;
            for (int k = std::max(0, a); k <= std::min(M_ - 1, b); k++)
                db.push_back(10.0f * std::log10((float)(psd_[(size_t)k] / activeSeg_) + 1e-30f));
            const CombResult c = measureComb(db.data(), (int)db.size(), fb);
            lastComb_ = c;
            const bool inWindow = c.bestHz >= kSpacingLo && c.bestHz <= kSpacingHi;
            if (inWindow && c.varaP < kCombMin && activeSeg_ >= segTarget_ && segTarget_ * 2 <= kSegsMax) {
                segTarget_ *= 2; evalMaxS_ = kEvalMaxLongS; evals_--; t.combAt = -1e18;   // ★ keep averaging (see kSegsMax)
                return;
            }
            const bool pass = c.bestHz >= kSpacingLo && c.bestHz <= kSpacingHi && c.varaP >= kCombMin;
            const float score = pass ? std::min(1.0f, (c.varaP - 0.04f) / (kCombFull - 0.04f)) : 0.0f;
            t.comb = std::max(0.0f, score);
            t.spacing = pass ? c.varaHz : c.bestHz;
            if (pass) evalsPass_++;
        }
        // too few on-segments: no verdict either way — the old comb score stands, a fresh try after kCombFreshS / 4
        else t.combAt = now - kCombFreshS * 0.75;
        active_ = -1; activeSeg_ = 0; ring_.clear(); std::fill(psd_.begin(), psd_.end(), 0.0);
    }

    void publish(double now) {
        std::vector<Tag> out;
        for (auto& t : tracks_) {
            t.conf = kShapeW * t.shape + (1.0f - kShapeW) * t.comb;
            const bool alive = now - t.lastOnS <= kHoldS;
            const bool pattern = bursts(t, now) >= 2 || onTime(t, now) >= 10.0;
            if (!t.tagged && alive && pattern && t.conf >= kTagConf) t.tagged = true;
            else if (t.tagged && (!alive || t.conf < kDropConf)) t.tagged = false;
            if (t.tagged) out.push_back({ t.id, t.hz, kClassHz[t.cls], t.conf, t.spacing });
        }
        std::string j = "{\"type\":\"sigtags\",\"v\":1,\"tags\":[";
        for (size_t k = 0; k < out.size(); k++) {
            char b[160];
            std::snprintf(b, sizeof b, "%s{\"id\":%d,\"hz\":%.0f,\"bw\":%.0f,\"k\":\"vara\",\"c\":%.2f}",
                          k ? "," : "", out[k].id, out[k].hz, out[k].bw, out[k].conf);
            j += b;
        }
        j += "]}";
        std::lock_guard<std::mutex> lk(mtx_);
        tags_.swap(out); json_.swap(j);
    }

    // stage 1
    double lastRowS_ = -1e18, rowCentre_ = 0, rowBinHz_ = 0;
    std::vector<float> blk_, floor_, tmp_, tmp2_;
    std::vector<Run> seen_;
    std::vector<Track> tracks_;
    int nextId_ = 1;
    double lastWidth_ = 0;
    // stage 2
    int active_ = -1, activeSeg_ = 0, M_ = 0, segTarget_ = kSegsWant;
    double evalMaxS_ = kEvalMaxS;
    double evalStartS_ = 0, segRate_ = 0, sliceCentre_ = 0;
    std::unique_ptr<vibedsp::ComplexFFT> fft_;
    std::vector<float> win_, pw_;
    std::vector<cf32> ring_, buf_, out_;
    std::vector<double> psd_;
    CombResult lastComb_;
    long long rej_[5] = { 0, 0, 0, 0, 0 };   // stage 1's rejections: width, sides, flatness, fill, level
    long long rows_ = 0, segsTotal_ = 0, evals_ = 0, evalsPass_ = 0, sliceSamples_ = 0;
    // published
    mutable std::mutex mtx_;
    std::vector<Tag> tags_;
    std::string json_ = kEmpty;
};

}  // namespace varatag
}  // namespace vibe
