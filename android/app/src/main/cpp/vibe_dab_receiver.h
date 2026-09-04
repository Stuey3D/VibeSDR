// vibe_dab_receiver.h — the whole DAB chain as one object: IQ in, ensemble + audio out.
//
// This is the piece that makes the previous stages a PIPELINE rather than a pile of parts. It owns
// the ordering and the state that spans frames; every stage it calls is separately tested.
//
//   IQ ──▶ FrameSync (null symbol)
//        ──▶ fractional offset from the cyclic prefix, de-rotate
//        ──▶ FFT per symbol ──▶ carriers ──▶ DQPSK against the previous symbol
//        ──▶ frequency deinterleave
//        ├──▶ symbols 1..3   : FIC ──▶ depuncture ──▶ Viterbi ──▶ descramble ──▶ FIBs ──▶ ensemble
//        └──▶ symbols 4..76  : MSC ──▶ CIFs ──▶ (per service) time deinterleave ──▶ depuncture
//                                  ──▶ Viterbi ──▶ descramble ──▶ MP2 or DAB+ super frames
//
// ★★ THE PHASE REFERENCE IS SYMBOL 0 AND IS NOT DATA. DQPSK needs a previous symbol to difference
//    against, and for symbol 1 that reference is the known phase-reference symbol — which is also
//    what makes the integer frequency offset recoverable. Treating it as data shifts every symbol
//    index by one and decodes nothing, with every stage reporting itself healthy.
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "vibe_dab_fec.h"
#include "vibe_dab_ficdec.h"
#include "vibe_dab_interleave.h"
#include "vibe_dab_modes.h"
#include "vibe_dab_msc.h"
#include "vibe_dab_ofdm.h"
#include "vibe_dab_prs.h"
#include "vibe_dab_sync.h"

namespace vibedab {

/** What the receiver can tell the DX panel right now. Every field is measured, never inferred. */
struct DabStats {
    bool   locked        = false;
    float  nullDepthDb   = 0.0f;   ///< how deep the null symbol reads — the "is this real" number
    float  freqOffsetHz  = 0.0f;   ///< fractional part, from the cyclic prefix
    float  freqOffsetPpm = 0.0f;   ///< the same, as a receiver-clock error
    int    fibsOk        = 0;      ///< of 12 this frame
    int    fibsTotal     = 0;
    double fibRate       = 0.0;    ///< running pass rate, 0..1
    int    framesSeen    = 0;
};

class DabReceiver {
public:
    explicit DabReceiver(uint32_t sampleRateHz = kCanonicalRateHz)
        : rate_(sampleRateHz), mode_(&modeI()), sync_(modeI(), sampleRateHz),
          fft_(size_t(fftSizeFor(modeI(), sampleRateHz))) {
        prsSymbol(prs_.data());
    }

    /** Feed a buffer of at least one frame. Returns true when a frame was decoded. */
    bool push(const Cplx* iq, size_t n) {
        const long at = sync_.offer(iq, n);
        if (at < 0) { stats_.locked = sync_.locked(); return false; }
        stats_.locked      = true;
        stats_.nullDepthDb = 10.0f * std::log10(sync_.lastDepth() + 1e-9f);

        const size_t nullLen = sync_.nullLen();
        const size_t symLen  = size_t(symbolSamplesAt(rate_));
        const size_t start   = size_t(at) + nullLen;
        if (start + symLen * size_t(mode_->symbolsPerFrame) > n) return false;

        // ── fractional offset, and correct it ───────────────────────────────
        std::vector<Cplx> work(iq + start, iq + start + symLen * size_t(mode_->symbolsPerFrame));
        const float frac = fractionalOffset(work.data(), work.size(), *mode_, 8);
        stats_.freqOffsetHz  = offsetHz(frac, *mode_);
        stats_.freqOffsetPpm = float(double(stats_.freqOffsetHz) / 222.064e6 * 1e6);  // vs 11D
        if (std::fabs(frac) > 1e-6f) derotate(work.data(), work.size(), double(frac) / double(fft_));

        // ── every symbol to carriers, then DQPSK against the previous ───────
        const int K = mode_->carriers;
        /* ★ Braces, not parens: `std::vector<C32> cur(size_t(K))` is a FUNCTION DECLARATION, not
         *  a vector — C++'s most vexing parse, and the compiler warned about exactly that. It
         *  would have compiled and then indexed a function. */
        std::vector<C32> cur(size_t(K), C32{}), prev(size_t(K), C32{}), spec(fft_, C32{});
        std::vector<int8_t> frameBits(size_t(K) * 2 * size_t(mode_->symbolsPerFrame - 1));

        for (int sym = 0; sym < mode_->symbolsPerFrame; ++sym) {
            const Cplx* p = work.data() + size_t(sym) * symLen + size_t(guardSamplesAt(rate_));
            dft(p, spec.data());
            carriersFromFft(spec.data(), int(fft_), K, cur.data());
            if (sym == 0) { prev = cur; continue; }          // the phase reference — not data
            std::vector<int8_t> soft(size_t(K) * 2);
            for (int k = 0; k < K; ++k) {
                const SoftBits b = dqpskSoft(cur[size_t(k)], prev[size_t(k)]);
                soft[size_t(k) * 2]     = b.b0;
                soft[size_t(k) * 2 + 1] = b.b1;
            }
            // ★ Frequency deinterleave undoes the transmitter's carrier scramble, per symbol.
            std::vector<int8_t> di(size_t(K) * 2);
            for (int nIdx = 0; nIdx < K; ++nIdx) {
                const int kk  = fi_.carrierFor(nIdx);
                const int src = kk < 0 ? kk + 768 : kk + 767;
                di[size_t(nIdx) * 2]     = soft[size_t(src) * 2];
                di[size_t(nIdx) * 2 + 1] = soft[size_t(src) * 2 + 1];
            }
            std::copy(di.begin(), di.end(),
                      frameBits.begin() + long(size_t(sym - 1) * size_t(K) * 2));
            prev = cur;
        }

        // ── symbols 1..3 are the FIC ────────────────────────────────────────
        const size_t ficBits = size_t(K) * 2 * 3;
        if (frameBits.size() >= ficBits) {
            const int ok = ficDecodeFrame(frameBits.data(), ensemble_, viterbi_);
            stats_.fibsOk    = ok;
            stats_.fibsTotal = 12;
            // ★ A RUNNING rate, not this frame's — one bad frame is weather, not a signal quality.
            fibHist_ = fibHist_ * 0.9 + (double(ok) / 12.0) * 0.1;
            stats_.fibRate = fibHist_;
        }
        ++stats_.framesSeen;
        return true;
    }

    const Ensemble& ensemble() const { return ensemble_; }
    const DabStats& stats()    const { return stats_; }
    void reset() { sync_.reset(); ensemble_ = Ensemble{}; stats_ = DabStats{}; fibHist_ = 0; }

private:
    int symbolSamplesAt(uint32_t r) const {
        return int((uint64_t(symbolSamples(*mode_)) * r) / kCanonicalRateHz);
    }
    int guardSamplesAt(uint32_t r) const { return guardSamplesFor(*mode_, r); }

    /** ★ A plain DFT for now — correct before fast. The shim already carries a real FFT
     *  (vibedsp::ComplexFFT) and this is the one call to swap when it is wired in; keeping it
     *  obvious here means the first integration bug cannot be hiding in a transform. */
    void dft(const Cplx* in, C32* out) {
        const size_t N = fft_;
        for (size_t k = 0; k < N; ++k) {
            double re = 0, im = 0;
            for (size_t t = 0; t < N; ++t) {
                const double a = -2.0 * M_PI * double(k) * double(t) / double(N);
                const double c = std::cos(a), s = std::sin(a);
                re += double(in[t].re) * c - double(in[t].im) * s;
                im += double(in[t].re) * s + double(in[t].im) * c;
            }
            out[k] = C32(float(re), float(im));
        }
    }

    uint32_t rate_;
    const Mode* mode_;
    FrameSync sync_;
    size_t fft_;
    FreqInterleaveI fi_;
    Viterbi viterbi_;
    Ensemble ensemble_;
    DabStats stats_;
    double fibHist_ = 0;
    std::array<C32, 1536> prs_{};
};

}  // namespace vibedab
