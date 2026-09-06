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
#include <cstdlib>

#include <atomic>
#include <cstdlib>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

#include "vibe_dab_fec.h"
#include "vibe_dab_fft.h"
#include "vibe_dab_ficdec.h"
#include "vibe_dab_interleave.h"
#include "vibe_dab_modes.h"
#include "vibe_dab_msc.h"
#include "vibe_dab_ofdm.h"
#include "vibe_dab_prs.h"
#include "vibe_dab_sync.h"

namespace vibedab {

/** ★ The MSC erasure threshold, as a fraction of the PRS correlation's own running reference.
 *  0 disables erasure entirely. Live-settable so it can be A/B'd on a single lock — see the note
 *  at the test itself. Seeded from VIBE_DAB_NOERASE / VIBE_DAB_ERASE_FRAC on first use. */
inline std::atomic<float>& dabEraseFrac() {
    static std::atomic<float> v{
        std::getenv("VIBE_DAB_NOERASE") ? 0.0f
      : (std::getenv("VIBE_DAB_ERASE_FRAC") ? float(std::atof(std::getenv("VIBE_DAB_ERASE_FRAC")))
                                            : 0.25f) };
    return v;
}


/** What the receiver can tell the DX panel right now. Every field is measured, never inferred. */
struct DabStats {
    bool   locked        = false;
    int    reacquires    = 0;      ///< times the sync was thrown away and re-taken
    float  nullDepthDb   = 0.0f;   ///< how deep the null symbol reads — the "is this real" number
    float  freqOffsetHz  = 0.0f;   ///< fractional part, from the cyclic prefix
    float  freqOffsetPpm = 0.0f;   ///< the same, as a receiver-clock error
    int    fibsOk        = 0;      ///< of 12 this frame
    int    fibsTotal     = 0;
    double fibRate       = 0.0;    ///< running pass rate, 0..1
    int    framesSeen    = 0;
    int    intOffsetCarriers = 0;   ///< whole carriers of offset, from the phase reference
    /** ★ Frames handed to the decoders as ERASURES because the phase reference said the window
     *  was wrong. A receiver that never erases is one that is guessing; one that erases often is
     *  losing sync. Both are worth seeing. */
    int    erasedFrames      = 0;
    /** ★★★ NOT A QUALITY — A LEVEL TIMES A QUALITY, AND THE DIFFERENCE HAS TEETH.
     *  The reference carriers are unit magnitude by construction, so this is
     *      |SUM received_carrier x conj(reference_carrier)| / K
     *  which scales with the RECEIVED AMPLITUDE. It moves when the AGC moves, with no change in
     *  the signal at all. Its absolute value means nothing; only its ratio to its own recent
     *  average does, which is exactly how the erasure test below uses it — and is why that ratio
     *  is now published beside it rather than leaving a bare number on screen for a human to
     *  judge against a scale that shifts underneath them. */
    float  prsCorrelation    = 0.0f;
    float  prsRef            = 0.0f;///< the running reference the erasure test compares against
};

class DabReceiver {
public:
    explicit DabReceiver(uint32_t sampleRateHz = kCanonicalRateHz)
        : rate_(sampleRateHz), mode_(&modeI()), sync_(modeI(), sampleRateHz),
          fft_(size_t(fftSizeFor(modeI(), sampleRateHz))) {
        fftp_ = std::make_unique<Fft>(fft_);
        prsSymbol(prs_.data());
    }

    /** Feed a buffer of at least one frame. Returns true when a frame was decoded. */
    bool push(const Cplx* iq, size_t n) {
        const long at = sync_.offer(iq, n);
        lastAt_ = at;
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
        stats_.freqOffsetPpm = float(double(stats_.freqOffsetHz) / centreHz_ * 1e6);
        if (std::fabs(frac) > 1e-6f) derotate(work.data(), work.size(), double(frac) / double(fft_));

        const int K = mode_->carriers;
        /* ★ Braces, not parens: `std::vector<C32> cur(size_t(K))` is a FUNCTION DECLARATION, not
         *  a vector — C++'s most vexing parse, and the compiler warned about exactly that. It
         *  would have compiled and then indexed a function. */
        std::vector<C32> cur(size_t(K), C32{}), prev(size_t(K), C32{}), spec(fft_, C32{});
        std::vector<int8_t> frameBits(size_t(K) * 2 * size_t(mode_->symbolsPerFrame - 1));

        /* ★★★ THE INTEGER FREQUENCY OFFSET — WITHOUT THIS, NOTHING DECODES.
         *
         *  The cyclic prefix resolves only a FRACTION of a carrier spacing (±500 Hz in Mode I).
         *  An RTL-SDR's crystal is typically tens of ppm out, and at 225 MHz even 1 ppm is 225 Hz
         *  — so a real dongle sits several WHOLE carriers away and every carrier lands in the
         *  wrong bin. On the first live capture this showed as a solid 22.3 dB null lock with
         *  0/12 FIBs: the frame structure was found perfectly and the contents were nonsense.
         *
         *  ★★ The phase reference symbol is the only thing in DAB whose carriers are known
         *     absolutely, which is exactly what makes it the tool for this: correlate the
         *     received symbol against it at each candidate shift and take the peak. It costs one
         *     transform we have already done.
         *  ★ Searched once per frame rather than tracked, because it cannot change quickly — a
         *    crystal does not drift a kilohertz between frames — and re-measuring is cheaper than
         *    being wrong after a retune.
         */
        /* ★★★ THE FFT WINDOW SITS IN THE MIDDLE OF THE GUARD INTERVAL, NOT AT THE END OF IT.
         *  The frame start comes from the null-symbol energy search and jitters by several
         *  samples frame to frame (measured on a 12B capture: deltas of -4..+5 on 40 % of frames).
         *  With the window at the very start of the useful part, every LATE frame takes its last
         *  samples from the NEXT symbol's prefix — inter-symbol interference on every carrier —
         *  while an early one is harmless. Half a guard interval earlier the window tolerates
         *  ±252 samples of timing error either way, and the linear phase ramp it puts across the
         *  carriers cancels in the DQPSK difference because every symbol gets the same ramp. This
         *  is also where a pre-echo (an earlier, weaker SFN path) does least damage. */
        const size_t winOff = size_t(guardSamplesAt(rate_)) - size_t(guardSamplesAt(rate_)) / 2;
        {
            const Cplx* p0 = work.data() + winOff;
            dft(p0, spec.data());
            std::vector<C32> got(size_t(K), C32{});
            carriersFromFft(spec.data(), int(fft_), K, got.data());
            /* ★★★ CORRELATE THE DIFFERENCE BETWEEN ADJACENT CARRIERS, NOT THE CARRIERS.
             *  A timing error of t samples multiplies carrier k by exp(-j2pi k t / 2048): four
             *  samples is three quarters of a turn across the band, and a direct correlation of
             *  received against reference carriers collapses under it — which is why the old
             *  "prsCorrelation" fell from ~10 to ~0.9 on jittered frames and frames were erased
             *  that were perfectly readable. The product of a carrier with the conjugate of its
             *  neighbour cancels the ramp (it is the same for both bar one step), so this figure
             *  measures the PHASE REFERENCE and nothing else. welle.io, dab-cmdline and DAB-Radio
             *  all do this; it took a comparison against them to see why ours did not. */
            std::vector<C32> D(size_t(K) - 1);
            for (int k = 0; k + 1 < K; ++k) D[size_t(k)] = got[size_t(k)] * std::conj(got[size_t(k) + 1]);
            if (prsDiff_.empty()) {
                prsDiff_.resize(size_t(K) - 1);
                for (int k = 0; k + 1 < K; ++k) prsDiff_[size_t(k)] = prs_[size_t(k)] * std::conj(prs_[size_t(k) + 1]);
            }
            float best = -1.0f; int bestShift = 0;
            const int span = 48;                       // ±48 carriers ≈ ±48 kHz ≈ ±210 ppm
            for (int d = -span; d <= span; ++d) {
                double re = 0, im = 0;
                for (int k = 0; k + 1 < K; ++k) {
                    const int src = k + d;
                    if (src < 0 || src + 1 >= K) continue;
                    const C32 a2 = D[size_t(src)];
                    const C32 b2 = prsDiff_[size_t(k)];
                    re += double(a2.real()) * b2.real() + double(a2.imag()) * b2.imag();
                    im += double(a2.imag()) * b2.real() - double(a2.real()) * b2.imag();
                }
                const float mag = float(std::sqrt(re * re + im * im));
                if (mag > best) { best = mag; bestShift = d; }
            }
            intShift_ = bestShift;
            stats_.intOffsetCarriers = bestShift;
            stats_.freqOffsetHz += float(bestShift) * float(mode_->spacingHz);
            stats_.freqOffsetPpm = float(double(stats_.freqOffsetHz) / centreHz_ * 1e6);
            stats_.prsCorrelation = best / float(K);
            /* ★★★ THE INTEGER OFFSET IS A FREQUENCY, SO IT IS REMOVED IN TIME, NOT BY RE-INDEXING
             *  BINS. Reading carrier k from bin k+d gets the amplitudes right and the PHASES
             *  wrong: an offset of d carriers advances every carrier by 2*pi*d*(Ts/Tu) between
             *  consecutive symbols, i.e. d x 504/2048 of a turn = 88.6 degrees per carrier of
             *  offset, and DQPSK differences that against nothing. The old bin shift therefore
             *  decoded only when d was 0 (or by luck a multiple of ~4), which the 1 ppm TCXO in
             *  the V4 hid completely; a dongle 20 ppm out would have locked perfectly and read
             *  nothing — exactly the dead-centre-tuning symptom of 2026-09-06. Found by comparing
             *  against the references, which all derotate the total offset in the time domain. */
            if (bestShift != 0)
                derotate(work.data(), work.size(), double(bestShift) / double(fft_));
        }

        // ── every symbol to carriers, then DQPSK against the previous ───────
        for (int sym = 0; sym < mode_->symbolsPerFrame; ++sym) {
            const Cplx* p = work.data() + size_t(sym) * symLen + winOff;
            dft(p, spec.data());
            carriersFromFft(spec.data(), int(fft_), K, cur.data());
            if (sym == 0) { prev = cur; continue; }          // the phase reference — not data
            /* ★★★ THE 2K BITS ARE NOT INTERLEAVED — THE FIRST K ARE THE REAL PARTS.
             *
             *  EN 300 401 clause 14.5, verbatim:
             *      q(l,n) = (1/√2)[ (1 - 2·p(l,n)) + j·(1 - 2·p(l,n+K)) ]
             *  So bit n is the REAL part of QPSK symbol n and bit n+K is its IMAGINARY part. The
             *  vector is two halves, not alternating pairs.
             *
             *  ★★ I had written p[2n] / p[2n+1], and on the first live capture that produced a
             *     rock-solid lock — 22.3 dB null, correct frequency offset, phase reference found
             *     at shift 0 — and 0 of 12 FIBs. Every stage reported itself healthy while the bit
             *     order was wrong, which is this project's most familiar failure shape. Only
             *     reading the clause settled it.
             *  ★ (1 - 2p): p=0 gives +1, so a POSITIVE soft value means bit 0 — which is the
             *    convention the Viterbi already expects.
             */
            /* ★★★ TWO PASSES, BECAUSE THE SCALE IS A PROPERTY OF THE SYMBOL, NOT THE CARRIER.
             *  The first pass forms the differential products and their mean magnitude; the
             *  second scales every carrier by that ONE number, so the relative strengths survive
             *  into the soft bits and the Viterbi can discount faded carriers. See
             *  dqpskSoftScaled — normalising each carrier to full scale, as this used to, is the
             *  same as telling the decoder every carrier is equally trustworthy. */
            std::vector<int8_t> di(size_t(K) * 2);
            prod_.resize(size_t(K));
            double magSum = 0.0;
            for (int nIdx = 0; nIdx < K; ++nIdx) {
                const int kk  = fi_.carrierFor(nIdx);        // carrier that carried symbol nIdx
                const int src = kk < 0 ? kk + 768 : kk + 767;
                const C32 p = dqpskProduct(cur[size_t(src)], prev[size_t(src)]);
                prod_[size_t(nIdx)] = p;
                magSum += std::sqrt(double(p.real()) * p.real() + double(p.imag()) * p.imag());
            }
            const double avg = magSum / double(K);
            const float invAvg = avg > 1e-12 ? float(1.0 / avg) : 0.0f;
            for (int nIdx = 0; nIdx < K; ++nIdx) {
                const SoftBits b = dqpskSoftScaled(prod_[size_t(nIdx)], invAvg);
                di[size_t(nIdx)]              = b.b0;        // real  -> p[n]
                di[size_t(K) + size_t(nIdx)]  = b.b1;        // imag  -> p[n+K]
            }
            std::copy(di.begin(), di.end(),
                      frameBits.begin() + long(size_t(sym - 1) * size_t(K) * 2));
            prev = cur;
        }

        /* ★★★ A FRAME WE DO NOT TRUST IS AN ERASURE, NOT A GUESS.
         *  Time interleaving exists so that a burst is spread thin enough for the convolutional
         *  code to CORRECT it — one lost frame out of sixteen in the deinterleaver's memory is
         *  well within what rate-1/2-ish DAB coding handles. It only works if the decoder is told
         *  those bits are unknown. Hand it hard, confident nonsense instead and the Viterbi is
         *  actively misled: it will happily find a wrong path that agrees with the garbage.
         *  ★★★ MEASURED ON CAPTURED AIR (12B, 30 s, dab-offline): ONE frame in 311 lost sync —
         *      phase-reference correlation collapsing from 9.5 to 0.90 with the whole FIC failing
         *      0/12 — and it destroyed THIRTEEN consecutive audio frames, ~300 ms, which is the
         *      squeal Stuart has been hearing. The event is one frame; the damage was ours.
         *  ★★★ 0.25, MEASURED — NOT CHOSEN. Swept against 60 s of captured air carrying three real
         *      burst events, decoding BBC Radio 1 on 12B:
         *          off   0 erased of 624   MP2 bad 2.3%
         *          0.15 12 erased          0.6%
         *          0.25 20 erased          0.1%   <- here
         *          0.35 34 erased          1.3%   <- what I first guessed at, and shipped
         *          0.50 43 erased          1.5%
         *          0.65 72 erased          3.1%
         *      Erasing too much is as harmful as erasing too little: past the optimum the
         *      deinterleaver window starts drawing on several erased frames at once and the code
         *      runs out of usable bits. My guess was 13x worse than the measurement.
         *  ★ The threshold is RELATIVE, because the correlation's scale follows the signal. A
         *    frame at a third of the running reference is not a fade — a fade moves the null
         *    depth too — it is a window in the wrong place. */
        if (prsRef_ <= 0.0f) prsRef_ = stats_.prsCorrelation;
        else if (stats_.prsCorrelation > prsRef_)
             prsRef_ = prsRef_ * 0.90f + stats_.prsCorrelation * 0.10f;   // rise quickly
        else prsRef_ = prsRef_ * 0.99f + stats_.prsCorrelation * 0.01f;   // fall slowly
        /* ★★★ SETTABLE WHILE IT RUNS, BECAUSE A RESTART DESTROYS THE COMPARISON. Changing this
         *  by rebuilding means a new process, a new AGC climb from the tuner's minimum (~50 s) and
         *  a fresh acquisition — and on a multiplex that varies minute to minute that confound is
         *  larger than the effect being measured. It already ruined one A/B today: 1.537 vs 2.048
         *  MHz measured nothing but 10C's own fading, twice, before the filter turned out never to
         *  have been applied at all. Toggled live, both arms share one lock, one gain, one minute
         *  of air. ★ Env still seeds it, so an unattended server can start either way. */
        const float kEraseFrac = dabEraseFrac().load(std::memory_order_relaxed);
        stats_.prsRef = prsRef_;
        const bool untrusted = kEraseFrac > 0.0f
                            && (prsRef_ > 0.0f && stats_.prsCorrelation < kEraseFrac * prsRef_);
        const size_t ficBits = size_t(K) * 2 * 3;
        if (untrusted) {
            ++stats_.erasedFrames;
            /* ★ ONLY the MSC is erased. The FIC is not decoded AT ALL on a frame like this —
             *  its FIBs are CRC-checked, but a window in the wrong place produces garbage that
             *  occasionally passes, and one bad FIG rewrites the ensemble database (a
             *  sub-channel's start or size) for every service. Nor is it counted as a FIC
             *  failure: it is a frame we declined to read, and reporting it as 0/12 dragged the
             *  displayed FIB rate from 1.00 to 0.82 — a quality figure Stuart reads off the
             *  screen, describing a fault that is no longer happening. */
            if (frameBits.size() > ficBits)
                std::fill(frameBits.begin() + long(ficBits), frameBits.end(), int8_t(0));
        }

        // ── symbols 1..3 are the FIC ────────────────────────────────────────
        if (!untrusted && frameBits.size() >= ficBits) {
            ficBits_.assign(frameBits.begin(), frameBits.begin() + long(ficBits));
            const int ok = ficDecodeFrame(frameBits.data(), ensemble_, viterbi_);
            stats_.fibsOk    = ok;
            stats_.fibsTotal = 12;
            // ★ A RUNNING rate, not this frame's — one bad frame is weather, not a signal quality.
            fibHist_ = fibHist_ * 0.9 + (double(ok) / 12.0) * 0.1;
            stats_.fibRate = fibHist_;
            /* ★★★ RE-ACQUIRE WHEN THE LOCK IS GENUINELY GONE — NOTHING EVER DID.
             *  Acquisition is TRACKED deliberately (re-taking it every frame costs a frame
             *  whenever some other dip is deeper — see workerLoop), but nothing re-took it EVER,
             *  so a lock lost to a fade, an AGC step or a retune was lost permanently. Stuart:
             *  "the BBC multiplex showed all of its stations then promptly cleared them all away
             *  and now isnt populating again ... tune away and back again reestablishes the
             *  lock" — because that was the only thing that called reset().
             *  ★★ THE THRESHOLD IS THE DESIGN. A frame or two of nothing is weather on a marginal
             *     mux and must not throw the lock away. But the FIC is the most heavily protected
             *     thing DAB transmits: if NOT ONE of twelve FIBs reads for a second and a half
             *     while frames keep arriving, we are synchronised to something that is no longer
             *     this ensemble. */
            static constexpr int kDeadFramesToReacquire = 62;      // ~1.5 s of 24 ms frames
            if (ok > 0) ficDead_ = 0;
            else if (++ficDead_ >= kDeadFramesToReacquire) {
                ficDead_ = 0;
                ++stats_.reacquires;
                sync_.reset();
                fibHist_ = 0.0;
            }
        }
        /* ── the MSC: symbols 4..76 carry four CIFs ─────────────────────────
         *  ★ 72 data symbols x 3072 bits = 221 184 = 4 x 55 296, which is the arithmetic that
         *    says the split is right. A CIF is 864 capacity units of 64 bits and arrives every
         *    24 ms, four to a Mode I frame. */
        if (frameBits.size() >= ficBits + size_t(kCifBits) * 4) {
            const int8_t* msc = frameBits.data() + ficBits;
            for (int c = 0; c < kCifsPerFrameI; ++c)
                pumpService(msc + size_t(c) * size_t(kCifBits));
        }

        ++stats_.framesSeen;
        return true;
    }

    float prsRef_ = 0.0f;      ///< running reference for the phase-reference correlation
    long lastAt_ = -1;

    /** ★ The FIC soft bits of the last frame — for diagnostics against a live signal, where the
     *  question "is the chain wrong before or after the Viterbi?" is otherwise unanswerable. */
    const std::vector<int8_t>& lastFicBits() const { return ficBits_; }

    /** Choose which service to decode audio for. Returns false when it is not in the ensemble
     *  yet, or has no audio component we can reach. */
    bool selectService(uint32_t sid) {
        auto it = ensemble_.services.find(sid);
        if (it == ensemble_.services.end() || it->second.components.empty()) return false;
        const ServiceComponent* pick = nullptr;
        for (const auto& c : it->second.components)
            if (c.subChId >= 0 && (pick == nullptr || c.primary)) pick = &c;
        if (!pick) return false;
        auto sc = ensemble_.subChannels.find(pick->subChId);
        /* ★ NOT `sizeCu <= 0`. A short-form (UEP) sub-channel legitimately reports size 0 — its
         *  capacity comes from table 8, not from the bitstream — so that guard silently rejected
         *  every BBC Layer II service while the station list looked perfect. */
        if (sc == ensemble_.subChannels.end()) return false;
        if (sc->second.eep && sc->second.sizeCu <= 0) return false;
        sel_      = sc->second;
        selType_  = pick->scType;
        selSid_   = sid;
        /* ★★ A sub-channel's CU size fixes its bit rate: 1 CU = 64 bits per 24 ms = 8 kbit/s at
         *  rate 1/2 … but the honest derivation is through the PROTECTION PROFILE, because the
         *  coded size is sizeCu x 64 and the profile says how much of that is data. */
        prof_ = {}; uprof_ = {}; dataBits_ = 0;
        int codedBits = 0;
        if (!sel_.eep) {
            /* ★★★ UEP — WHICH IS WHAT THE BBC ACTUALLY USES. The short form of FIG 0/1 carries a
             *  6-bit TABLE INDEX rather than a size, so sizeCu arrives as 0 and everything must
             *  come from table 8: the capacity, the protection level and the bit rate. Reading
             *  the real 12B multiplex, every Layer II service is short-form — so without this the
             *  station list decodes perfectly and not one service can be played. */
            if (sel_.protLevel < 0 || sel_.protLevel > 63) return false;
            const UepIndex& ix = kUepIndex[sel_.protLevel];
            uprof_ = uepProfile(ix.bitrateKbps, ix.protLevel);
            if (!uprof_.valid) return false;
            sel_.sizeCu = ix.sizeCu;
            codedBits   = ix.sizeCu * kCuBits;
            dataBits_   = ix.bitrateKbps * 24;          // kbit/s x 24 ms
            bitrate_    = ix.bitrateKbps;
        } else {
            /* ★★★ THE OPTION IS ON AIR — USE IT, DO NOT GUESS IT. This tried EEP-A and then
             *  EEP-B and kept whichever matched the coded size, which two profiles can both do;
             *  a wrong choice depunctures with the wrong vector and every frame fails. The FIG
             *  0/1 parser now stores it (it was being folded into protLevel, which is the bug
             *  that took 11D out entirely). */
            codedBits = sel_.sizeCu * kCuBits;
            const bool eepB = (sel_.option == 1);
            const int  mult = eepB ? 32 : 8;
            for (int n = 1; n <= 24 && !prof_.valid; ++n) {
                const EepProfile p = eepProfile(sel_.protLevel + 1, eepB, n);
                if (p.valid && eepCodedBits(p) == codedBits) {
                    prof_ = p; dataBits_ = n * mult * 24; bitrate_ = n * mult;
                }
            }
            if (!prof_.valid) return false;
        }
        deint_ = std::make_unique<TimeDeinterleaver>(size_t(codedBits));
        audio_.clear();
        return prof_.valid || uprof_.valid;
    }

    /** Logical frames of decoded audio bytes — MP2 frames, or DAB+ super frame fifths. */
    const std::vector<std::vector<uint8_t>>& audioFrames() const { return audio_; }

    /** ★★ TAKE the frames rather than index into them. The buffer is a bounded ring, so a caller
     *  that remembers "I have consumed N" silently loses frames the moment it wraps — which is
     *  exactly what happened: ten seconds of capture yielded 1.5 seconds of audio, with no error
     *  anywhere. Moving them out makes that impossible to get wrong. */
    std::vector<std::vector<uint8_t>> takeAudioFrames() {
        std::vector<std::vector<uint8_t>> out;
        out.swap(audio_);
        return out;
    }
    int  selectedType() const { return selType_; }      ///< 0 = MP2, 63 = DAB+
    const EepProfile& profile() const { return prof_; }
    const UepProfile& uepProf() const { return uprof_; }
    int  serviceBitrate() const { return bitrate_; }

    const Ensemble& ensemble() const { return ensemble_; }
    const DabStats& stats()    const { return stats_; }
    void reset() { sync_.reset(); ensemble_ = Ensemble{}; stats_ = DabStats{}; fibHist_ = 0; }
    /** ★ The ppm figure was computed against 222.064 MHz (11D) whatever block was tuned — 8 %
     *  wrong at 5A, invisible on 12B. The service tells us the block; this is what it divides by. */
    void setCentreHz(double hz) { if (hz > 1e6) centreHz_ = hz; }
    /** ★ Drop the frame-timing prediction but KEEP the ensemble. For a caller that steps through a
     *  capture window by window rather than streaming — the tracker's prediction is relative to
     *  the buffer it was given, so a new window needs a fresh acquisition. */
    void resetSync() { sync_.reset(); }
    /** Where the last push() found the null, or -1. The caller must consume THROUGH the frame it
     *  decoded, not a fixed amount from the front — see DabService::feed. */
    long lastFrameStart() const { return lastAt_; }
    /** Tell the sync that `n` samples were dropped from the front of the buffer. See consumed(). */
    void syncConsumed(size_t n) { sync_.consumed(n); }

private:
    int symbolSamplesAt(uint32_t r) const {
        return int((uint64_t(symbolSamples(*mode_)) * r) / kCanonicalRateHz);
    }
    int guardSamplesAt(uint32_t r) const { return guardSamplesFor(*mode_, r); }

    /** ★ Was a plain O(N^2) DFT, deliberately, so the first integration bug could not be hiding
     *  in a transform. That paid off — the bugs were in bit ordering, not the maths — and now the
     *  transform can be fast: 2048 points radix-2, ~11 000 operations instead of 4.2 million. */
    void dft(const Cplx* in, C32* out) {
        buf_.assign(fft_, C32{});
        for (size_t i = 0; i < fft_; ++i) buf_[i] = C32(in[i].re, in[i].im);
        fftp_->forward(buf_.data());
        for (size_t i = 0; i < fft_; ++i) out[i] = buf_[i];
    }

    /** One CIF: pull our sub-channel's capacity units out, deinterleave, decode. */
    void pumpService(const int8_t* cif) {
        if ((!prof_.valid && !uprof_.valid) || !deint_) return;
        const size_t coded = size_t(sel_.sizeCu) * size_t(kCuBits);
        const int8_t* start = cif + size_t(sel_.startCu) * size_t(kCuBits);
        const std::vector<int8_t>& di = deint_->push(start);
        /* ★★ 15 CIFs of latency before the first complete logical frame — that is the interleaving
         *  depth, not an inefficiency. Emitting audio early means emitting frames with holes. */
        if (!deint_->ready()) return;

        std::vector<int8_t> mother(size_t(dataBits_ + 6) * 4, 0);
        if (uprof_.valid) uepDepuncture(di.data(), coded, uprof_, mother.data(), mother.size());
        else              eepDepuncture(di.data(), coded, prof_,  mother.data(), mother.size());
        std::vector<uint8_t> bits = viterbi_.decode(mother.data(), size_t(dataBits_));
        EnergyDispersal ed; ed.apply(bits.data(), bits.size());

        std::vector<uint8_t> bytes(bits.size() / 8);
        for (size_t i = 0; i < bytes.size(); ++i) {
            uint8_t b = 0;
            for (int k = 0; k < 8; ++k) b = uint8_t((b << 1) | (bits[i * 8 + size_t(k)] & 1));
            bytes[i] = b;
        }
        audio_.push_back(std::move(bytes));
        if (audio_.size() > 64) audio_.erase(audio_.begin());     // bounded
    }

    uint32_t rate_;
    std::unique_ptr<Fft> fftp_;
    std::vector<C32> buf_;
    const Mode* mode_;
    FrameSync sync_;
    size_t fft_;
    FreqInterleaveI fi_;
    Viterbi viterbi_;
    Ensemble ensemble_;
    DabStats stats_;
    double fibHist_ = 0;
    double centreHz_ = 222.064e6;   ///< the tuned block, for the ppm readout
    int    intShift_ = 0;
    std::vector<C32>    prsDiff_;   // reference adjacent-carrier products, built once
    std::vector<C32>    prod_;      // per-symbol differential products — see the CSI note
    std::vector<int8_t> ficBits_;
    SubChannel sel_{};
    EepProfile prof_{};
    UepProfile uprof_{};
    int bitrate_ = 0;
    int selType_ = -1, dataBits_ = 0;
    uint32_t selSid_ = 0;
    int    ficDead_ = 0;      ///< consecutive frames with NOT ONE readable FIB
    std::unique_ptr<TimeDeinterleaver> deint_;
    std::vector<std::vector<uint8_t>> audio_;
    std::array<C32, 1536> prs_{};
};

}  // namespace vibedab
