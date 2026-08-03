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
    // ★★ 2x the output width normally — the crop ratio is always > 1/2, so the crop keeps at least
    //    bins_ REAL bins to downsample from and never has to stretch. The SHARED path takes a
    //    channel twice as wide as the view (so extract()'s band-edge roll-off lands outside it),
    //    which halves the ratio again, so it needs 4x. Cheap: the transform is small, and the
    //    channel rate doubles alongside the window, so the frame rate is unchanged.
    fftN_ = bins_ * (m == Method::Shared ? 4 : 2);
    fft_ = std::make_unique<ComplexFFT>(fftN_);
    win_.resize(fftN_);
    nuttallWindow(win_.data(), fftN_);
    db_.resize(fftN_);
    out_.resize(bins_);
    acc2_.assign(bins_, 0.0f);
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
        // ★★ TAKE A CHANNEL TWICE AS WIDE AS THE VIEW. extract() rolls its band edges off to keep
        //    the impulse response inside the overlap (see channelizer.cpp); halving the decimation
        //    puts that roll-off OUTSIDE the requested span, where the crop in push_() throws it
        //    away. So the user sees a flat passband and the block joins are still clean.
        if (d > 1) d /= 2;
        const int minChanBins = 32;
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
    // ★★★ DERIVE THE HOP AND THE WINDOW COUNT TOGETHER, then drive the emit off the COUNT.
    //     Frames per second is chanRate/(winPerFrame_*hop_), so both have to come from the same
    //     division or the rate is wrong by whatever integer truncation threw away — with a stride
    //     of 6250 and 2 windows, hop_ = 3125 is exact, but a SAMPLE gate comparing against 6250
    //     when 2 hops give 6250 is fine and 3 hops is not, and any remainder costs a whole extra
    //     hop: 20 fps becomes 13 (Stuart, 2026-08-03, measured at 2/3 of target).
    //     ★ winPerFrame_ is also how many windows get AVERAGED into each frame, so a wider hop cap
    //       buys quieter frames rather than being wasted.
    winPerFrame_ = std::max(1, (int)((emitStride_ + (fftN_ / 2) - 1) / (fftN_ / 2)));
    hop_         = std::max(1, (int)(emitStride_ / winPerFrame_));
    winCount_    = 0;

    if (method_ == Method::Direct) {
        nco_.setFreq(offsetHz_ / sampleRate_);   // mix the view centre down to DC
    } else {
        const double binHz = sampleRate_ / (double)kSharedFft;
        centreBin_ = (int)llround(offsetHz_ / binHz);
        chanBins_  = kSharedFft / decim_;
    }

    if (!sameShape) rebuild_();
    enabled_ = true;
    // ★ Say what this thing ACTUALLY believes. Everything here is derived from sampleRate_, which
    //   comes from the PIPELINE, not from the caller — so if the two ever disagree the offset maps
    //   to the wrong place and nothing upstream can show it.
    if (logCb_) logCb_(sampleRate_, offsetHz_, reqSpanHz_, spanHz_, decim_, centreBin_, chanBins_);
}

void ZoomSpectrum::rebuild_() {
    accN_ = 0; winCount_ = 0;

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
        if (accN_ < fftN_) continue;
        // Transform EVERY window; the rate gate decides when to SEND, not whether to compute.
        {
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
                acc2_[o] += best;                    // ★ ACCUMULATE, never discard
            }
            ++avgN_;
        }

        ++winCount_;
        // ★★★ AVERAGE THE WINDOWS BETWEEN EMITS — DO NOT THROW THEM AWAY. The rate gate used to
        //     drop whole windows at low frame rates, so anything brief that happened between two
        //     emitted frames simply never appeared: "the dropoff at the lower data rates is vastly
        //     more noticeable" (Stuart, 2026-08-02). The wide path never had this because it
        //     block-averages FFT_AVG frames. Averaging costs nothing extra, keeps every sample
        //     represented, and quietens the noise floor at exactly the rates where it looked worst.
        if (winCount_ >= winPerFrame_ && avgN_ > 0) {
            winCount_ = 0;
            const float inv = 1.0f / (float)avgN_;
            for (int o = 0; o < bins_; ++o) out_[o] = acc2_[o] * inv;
            std::fill(acc2_.begin(), acc2_.end(), 0.0f);
            avgN_ = 0;
            cb(out_.data(), bins_);
        }

        // ★★ OVERLAPPING WINDOWS — SLIDE, DO NOT RESET. Window LENGTH sets resolution; window RATE
        //    sets how often we have something to show. Emitting one frame per non-overlapping
        //    window tied them together, so deep zoom crawled whatever rate was asked for. The hop
        //    is FIXED (not rate-derived): dense windows feed the averaging above, and the gate
        //    alone decides the wire rate. Bins are unchanged either way.
        // ★★★ AS BIG A HOP AS THE FRAME RATE ALLOWS. Overlapping windows buy FRAMES, but heavily
        //     overlapped windows are nearly IDENTICAL, so averaging them buys almost no noise
        //     reduction — 24 windows at 94% overlap is worth about one and a half independent ones,
        //     which looks like integration and is not. It showed as speckle against UberSDR's
        //     smooth traces (Stuart, 2026-08-02). So: hop as large as the requested rate permits,
        //     capped at half a window so nothing is missed between frames.
        // ★★★ NO LOWER FLOOR — IT WAS THROTTLING THE FRAME RATE AT HIGH ZOOM. A floor of fftN_/16
        //     was added "so this cannot run away", but the arithmetic says it never could: frames
        //     per second is chanRate/hop, and with hop following the emit interval that is
        //     chanRate/(chanRate/rate) = THE REQUESTED RATE, at every zoom level. The FFT count is
        //     self-limiting. What the floor actually did was cap the hop as chanRate fell with
        //     zoom depth, so the frame rate fell with it — 20 fps asked for, 15 at a 7.8 kHz span
        //     and 7.6 deeper still (Stuart, 2026-08-03: "it seems to slow down as you zoom in").
        const int hopOut = hop_;
        std::memmove(acc_.data(), acc_.data() + hopOut, (size_t)(fftN_ - hopOut) * sizeof(cf32));
        accN_ = fftN_ - hopOut;
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
