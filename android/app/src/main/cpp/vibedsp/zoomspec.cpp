// VibeSDR — ZoomSpectrum: honest resolution at high zoom, by two routes.
//
// See the class note in vibedsp.h for WHY and for the measured cost of each method. This file is
// the how. Written 2026-08-02 alongside the Channelizer.
//
// ★ The shared output stage is the point of the design: BOTH methods end at "a narrow complex
//   stream at `sampleRate/decim_`", so everything after that — window, FFT, dB, rate gate — is
//   written once and is identical. That is also why the two give the SAME detail, which is worth
//   knowing before anyone writes user-facing copy claiming otherwise.
#include "vibedsp.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace vibedsp {

ZoomSpectrum::ZoomSpectrum(double sampleRate, Method m, int bins)
    : sampleRate_(sampleRate), method_(m), bins_(bins) {
    // 2x the output width — see the note on fft_ in the header. The crop back to the requested
    // span then always has at least bins_ real bins to downsample from.
    fftN_ = bins_ * 2;
    fft_ = std::make_unique<ComplexFFT>(fftN_);
    win_.resize(fftN_);
    nuttallWindow(win_.data(), fftN_);
    db_.resize(fftN_);
    out_.resize(bins_);
    acc_.resize(fftN_);
}

ZoomSpectrum::~ZoomSpectrum() = default;

void ZoomSpectrum::disable() {
    enabled_ = false;
    spanHz_ = 0.0; reqSpanHz_ = 0.0;
    decs_.clear();
    chan_.reset();
    accN_ = 0;
}

void ZoomSpectrum::configure(double offsetHz, double spanHz, double rateHz) {
    if (spanHz <= 0.0 || sampleRate_ <= 0.0) { disable(); return; }

    // ★ Decimation is a power of two: the Shared method needs chanBins to DIVIDE the forward FFT
    //   size, and the Direct method wants a cascade of small stages anyway. Pick the largest D
    //   whose output rate still covers the requested span — so the delivered span is the next
    //   achievable one AT OR ABOVE what was asked for, never below it (a view narrower than
    //   requested would silently hide signal the user had scrolled to).
    int d = 1;
    while (sampleRate_ / (double)(d * 2) >= spanHz && d < (1 << 14)) d *= 2;

    // The Shared method also needs at least a few bins per channel for the slice to mean
    // anything, and chanBins = fftSize/D, so D is capped by the forward FFT size.
    const int kSharedFft = 32768;
    if (method_ == Method::Shared) {
        const int minChanBins = 16;
        while (d > 1 && kSharedFft / d < minChanBins) d /= 2;
    }

    const double newSpan = sampleRate_ / (double)d;
    reqSpanHz_ = spanHz;               // what the FRAME must cover — the client scales by this
    const bool sameShape = enabled_ && d == decim_;
    // Retuning within the same decimation is cheap for Direct (just the NCO) but for Shared the
    // centre bin moves, which is also just a number. Only a change of D rebuilds filters.
    decim_    = d;
    offsetHz_ = offsetHz;
    spanHz_   = newSpan;
    rateHz_   = rateHz > 0 ? rateHz : 15.0;
    // Emit gate counts DECIMATED samples, so it is independent of the input block size.
    emitStride_ = std::max<long long>(1, (long long)llround((sampleRate_ / (double)decim_) / rateHz_));

    if (method_ == Method::Direct) {
        nco_.setFreq(offsetHz_ / sampleRate_);   // mix the view centre down to DC
    } else {
        const double binHz = sampleRate_ / (double)kSharedFft;
        centreBin_ = (int)llround(offsetHz_ / binHz);
        chanBins_  = kSharedFft / decim_;
    }

    if (!sameShape) rebuild_();
    enabled_ = true;
}

void ZoomSpectrum::rebuild_() {
    accN_ = 0; sinceEmit_ = 0;

    if (method_ == Method::Direct) {
        chan_.reset();
        decs_.clear();
        // ★★ CASCADE, not one filter. A single stage at D=512 would need a preposterous tap count
        //    running at the FULL input rate. Split into stages of 8 (then 4, then 2) and the early
        //    ones need only enough stopband to stop aliases folding in, while the tight filter
        //    runs LAST, at the lowest rate, where taps are cheapest. Same result, far less CPU.
        int rem = decim_;
        while (rem > 1) {
            int st = (rem % 8 == 0 && rem > 8) ? 8 : (rem % 4 == 0 && rem > 4) ? 4 : (rem % 2 == 0) ? 2 : rem;
            if (st > rem) st = rem;
            const bool last = (rem / st) == 1;
            // Last stage defines the view edge, so it gets the deep stopband; the others only
            // have to keep the fold-in below the noise.
            decs_.push_back(std::make_unique<FirDecimator>(
                last ? designLowpass(0.40, 0.10, /*deepStop=*/true)
                     : designLowpass(0.5 / st, 0.25 / st), st));
            rem /= st;
        }
    } else {
        decs_.clear();
        chan_ = std::make_unique<Channelizer>(32768);
        chanOut_.assign((size_t)std::max(1, chanBins_), cf32(0.0f, 0.0f));
    }
}

// Collect decimated samples until there are `fftN_` of them, then FFT, crop and emit — but only
// when the rate gate allows, so a high zoom does not quietly raise the frame rate.
void ZoomSpectrum::push_(const cf32* x, int n, const std::function<void(const float*, int)>& cb) {
    for (int i = 0; i < n; ++i) {
        acc_[accN_++] = x[i];
        ++sinceEmit_;
        if (accN_ < fftN_) continue;
        if (sinceEmit_ >= emitStride_) {
            sinceEmit_ = 0;
            const float scale = 1.0f / (float)((long long)fftN_ * fftN_);
            fft_->powerDbShifted(acc_.data(), win_.data(), db_.data(), scale);

            // ★★★ CROP TO THE SPAN THAT WAS ASKED FOR. db_ covers spanHz_ (a power-of-two
            //     decimation, so >= the request); the frame must cover EXACTLY reqSpanHz_ or the
            //     client's scale is wrong by the ratio between them, which reads as the spectrum
            //     drifting sideways the further you zoom in.
            int keep = (int)llround((double)fftN_ * (reqSpanHz_ / spanHz_));
            if (keep > fftN_) keep = fftN_;
            if (keep < bins_) keep = bins_;          // never stretch: downsample only
            const int lo = (fftN_ - keep) / 2;
            // Peak-hold down to the output width — the same rule the wide path uses, so a narrow
            // carrier cannot fall between two output bins and vanish.
            for (int o = 0; o < bins_; ++o) {
                const int a = lo + (int)((long long)o * keep / bins_);
                int b       = lo + (int)((long long)(o + 1) * keep / bins_);
                if (b <= a) b = a + 1;
                float best = -1e30f;
                for (int k = a; k < b && k < fftN_; ++k) if (db_[k] > best) best = db_[k];
                out_[o] = best;
            }
            cb(out_.data(), bins_);
        }
        accN_ = 0;      // non-overlapping windows: this is a display, not a detector
    }
}

void ZoomSpectrum::feed(const cf32* iq, int n,
                        const std::function<void(const float* db, int bins)>& onFrame) {
    if (!enabled_ || n <= 0) return;

    if (method_ == Method::Direct) {
        if ((int)mixBuf_.size() < n) { mixBuf_.resize(n); aBuf_.resize(n + 8); bBuf_.resize(n + 8); }
        nco_.mix(iq, mixBuf_.data(), n);                 // <-- runs at the FULL input rate
        cf32* src = mixBuf_.data();
        int   cnt = n;
        for (size_t s = 0; s < decs_.size(); ++s) {
            cf32* dst = (s % 2 == 0) ? aBuf_.data() : bBuf_.data();
            cnt = decs_[s]->process(src, cnt, dst);
            src = dst;
            if (cnt <= 0) return;
        }
        push_(src, cnt, onFrame);
        return;
    }

    // Shared: one forward FFT of the whole band, then a slice. The forward FFT here is the SAME
    // work every other listener's channel is taken from — that is the entire economic argument.
    chan_->feed(iq, n, [&](const cf32* bins, int) {
        const int got = chan_->extract(bins, centreBin_, chanBins_, chanOut_.data());
        if (got > 0) push_(chanOut_.data(), got, onFrame);
    });
}

}  // namespace vibedsp
