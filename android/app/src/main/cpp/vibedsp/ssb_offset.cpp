// SSB offset estimator, SSB shifter, recogniser auto-tune and feed — see ssb_offset.h for the why.
#include "ssb_offset.h"
#include "vibedsp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vibedsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

/* ★★★ THE GATE, MEASURED ON OFF-AIR CLIPS (vibeserver/test-ssb-offset-real.cpp, 2026-10-17).
 *    Trained on three VOLMET stations recorded at 11 dials at once (Shannon 5505 + 3413, RAF 5450: truth exact),
 *    checked on a HELD-OUT fourth (Shannon 8957), a 4-5 dB station that must never pass (RAF 11253), and eight
 *    40/80/20 m QSOs demodulated from busy-band I/Q at known dial offsets (held out too).
 *    • The old rule (strength rising 0.001/Hz with the offset) was for a ±300 Hz search, where a large answer was
 *      most likely a pitch alias. With the two stages an alias is refuted by the comb's below-carrier teeth or by
 *      the coarse veto instead, and what is left to tell apart is HOW CLEAR the winner is of its nearest in-range
 *      rival a pitch away — so the gate is a floor on strength AND on clarity, the same at every offset.
 *    • Beyond ±40 Hz the speech must also stand clear of the gaps (snrDb) and be backed by enough voiced frames:
 *      every believed-looking wrong answer in the training set came from the first second after a reset or from a
 *      window under ~8 dB.
 *    SLOW (10 s windows): confidence ≥ 0.4, clarity ≥ 0.3, beyond ±40 Hz snr ≥ 10 dB. Training: 84 of 99 windows
 *      pass, none wrong; held-out 8957: 30 of 33, none wrong; RAF 11253: none pass.
 *    FAST (an answer per 0.6 s of voice, 8 s memory): confidence ≥ 0.3, clarity ≥ 0.4, beyond ±40 Hz snr ≥ 8 dB
 *      and ≥ 30 frames (0.6 s of this voice's memory: 50 measured no safer on the clips, and after a QSO's change of
 *      speaker it held the first lean back a whole over). Training: 80 % of answers pass, none wrong; held-out 8957:
 *      79 %, none wrong; the held-out QSOs ≤ ±500 Hz: ~1 % of passes wrong (the other station), none wrong-signed.
 *      The test prints the current figures. */
bool SsbOffsetEstimate::passes() const {
    return valid && reconciled && frames >= minFrames && confidence >= gate.confidence && clarity >= gate.clarity
        && (std::fabs(offsetHz) <= gate.largeHz || (snrDb >= gate.largeSnrDb && frames >= gate.largeFrames));
}

SsbOffsetEstimator::Config SsbOffsetEstimator::Config::forProfile(Profile p) {
    Config c;
    if (p == Profile::Fast) {
        // ★ The binaural aid: an answer every ~1 s of speech while the knob turns. 40 ms frames
        //   (pitch moves less inside one), 3.9 Hz bins, 4 Hz offset steps — rougher, quicker.
        c.frame = 320; c.hop = 160; c.nfft = 2048;
        c.blockFrames = 25;              // 0.5 s selection blocks, the loudest 60 % kept
        c.keepFraction = 0.6f;
        c.windowVoicedSec = 0.6f; c.maxWindowSec = 3.0f; c.minVoicedSec = 0.3f;   // ~1 s of speech per answer
        c.dStepHz = 4;
        c.pitchSteps = 7; c.pitchStepHz = 1.0f;   // ±3 Hz in 1 Hz steps: as good on the clips, ~40 % cheaper
        c.minFrames = 20;                // 0.4 s of 40 ms frames at a 20 ms step
        // ★ 8 s of memory: measured 5 / 8 / 12 s on the clips — 8 s settles the deep side (where only intonation
        //   tells one pitch alias from the next) nearly as well as 12, and lets a new voice take over sooner.
        c.memoryVoicedSec = 8.0f;
        c.gate.confidence = 0.3f; c.gate.clarity = 0.4f; c.gate.largeSnrDb = 8.0f; c.gate.largeFrames = 30;
    }
    return c;
}

namespace {
constexpr float kBandHz = 25.0f;     // the coarse stage's spectrum: 25 Hz bands…
constexpr int   kBands = 120;        // …to 3 kHz
constexpr float kCoarseMinPeakDb = 7.0f;     // a voice that never stands this clear of the noise gives no position
constexpr float kCoarseEdgeDb = 6.0f;        // the edge: at least this far over the noise…
constexpr float kCoarseBelowPeakDb = 12.0f;  // …and within this of the voice's best band
constexpr float kCoarseTolHz = 50.0f;        // the edge's own error (25 Hz bands, the lowest harmonic's skirt)
constexpr float kTxLowCutMinHz = -50.0f;     // a transmitter's low cut: ESSB ~60 Hz, less the skirt and splatter below it…
constexpr float kTxLowCutMaxHz = 400.0f;     // …to a contest-style 400 Hz
constexpr float kReconcileSd = 1.0f;         // the comb's best OUTSIDE the interval beats the best inside by this (spreads): irreconcilable
constexpr double kSpeakerRatio = 1.6;        // FAST: a voice pitched this far (either way) from the memory's is another speaker…
constexpr double kParkedFade = 4.0;          // …and the other voice's memory, parked, fades this many times slower
}  // namespace

SsbOffsetEstimator::SsbOffsetEstimator(double inRate, Profile p)
    : SsbOffsetEstimator(inRate, Config::forProfile(p)) {}

SsbOffsetEstimator::SsbOffsetEstimator(double inRate, const Config& c) : c_(c) {
    if ((int)std::lround(inRate) != (int)std::lround(c_.rate))
        rs_ = std::make_unique<RationalResampler>((int)std::lround(inRate), (int)std::lround(c_.rate));
    fft_ = std::make_unique<RealFFT>(c_.nfft);
    win_.resize(c_.frame);
    for (int i = 0; i < c_.frame; ++i)   // numpy's np.hanning (symmetric), as the rig
        win_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * i / (c_.frame - 1)));
    fin_.assign(c_.nfft, 0.0f);
    spec_.resize(c_.nfft / 2 + 1);
    nd_ = (int)std::lround(2 * c_.dMaxHz / c_.dStepHz) + 1;
    ds_.resize(nd_);
    for (int j = 0; j < nd_; ++j) ds_[j] = -c_.dMaxHz + j * c_.dStepHz;
    acc_.assign(nd_, 0.0f); cur_.resize(nd_); sum_.resize(nd_); cnt_.resize(nd_);
    pv_.assign(kBands, 0.0); pq_.assign(kBands, 0.0);
    gamma_ = c_.memoryVoicedSec > 0 ? std::exp(-(c_.hop / c_.rate) / c_.memoryVoicedSec) : 1.0;
    gammaPark_ = c_.memoryVoicedSec > 0 ? std::exp(-(c_.hop / c_.rate) / (kParkedFade * c_.memoryVoicedSec)) : 1.0;
    blockFrames_.resize((size_t)c_.blockFrames * c_.frame);
    blockEnergy_.resize(c_.blockFrames);
}

SsbOffsetEstimator::~SsbOffsetEstimator() = default;

void SsbOffsetEstimator::reset() {
    if (rs_) rs_->reset();
    buf_.clear(); bufFill_ = 0; nBlock_ = 0;
    std::fill(acc_.begin(), acc_.end(), 0.0f);
    std::fill(pv_.begin(), pv_.end(), 0.0); std::fill(pq_.begin(), pq_.end(), 0.0);
    nv_ = nq_ = 0; weight_ = 0; framesTotal_ = 0; floorE_ = 0; f0Recent_ = 0;
    park_ = Memory{};
    used_ = 0; windowSamples_ = 0; snrSum_ = 0; snrW_ = 0; f0Sum_ = 0; f0N_ = 0;
    last_ = SsbOffsetEstimate{};
    count_ = 0;
}

void SsbOffsetEstimator::shiftAudio(float audioHz) {
    // The audio queued before the move belongs to the old dial: drop it, and the half-built selection block.
    if (rs_) rs_->reset();
    buf_.clear(); nBlock_ = 0;
    used_ = 0; windowSamples_ = 0;
    if (!std::isfinite(audioHz)) { reset(); return; }
    // The comb's evidence is over the offset in audio Hz: offset d becomes d + audioHz.
    const int sh = (int)std::lround(audioHz / c_.dStepHz);
    auto moveComb = [&](std::vector<float>& acc) {
        if (sh == 0 || acc.empty()) return;
        std::vector<float> a(nd_, 0.0f);
        for (int j = 0; j < nd_; ++j) { const int k = j - sh; if (k >= 0 && k < nd_) a[j] = acc[k]; }
        acc.swap(a);
    };
    // The voice's spectrum moves with it; the receiver's noise floor (pq_) does not. Move the voice's share only.
    const int bs = (int)std::lround(audioHz / kBandHz);
    auto moveVoice = [&](std::vector<double>& pv, double nv) {
        if (bs == 0 || nv <= 0 || pv.empty()) return;
        std::vector<double> v(kBands, 0.0);
        const double r = nq_ > 0 ? nv / nq_ : 0.0;    // the noise's share of pv, in pv's own units
        for (int b = 0; b < kBands; ++b) {
            const int k = b - bs;
            const double voice = k >= 0 && k < kBands ? std::max(0.0, pv[k] - r * pq_[k]) : 0.0;
            v[b] = voice + r * pq_[b];
        }
        pv.swap(v);
    };
    moveComb(acc_); moveVoice(pv_, nv_);
    moveComb(park_.acc); moveVoice(park_.pv, park_.nv);   // the parked voice moved too
}

float SsbOffsetEstimator::pendingVoicedSeconds() const { return (float)(used_ * (double)c_.hop / c_.rate); }

bool SsbOffsetEstimator::push(const float* x, int n) {
    const int before = count_;
    const float* src = x;
    int m = n;
    if (rs_) {
        rsOut_.resize((size_t)rs_->maxOut(n));
        m = rs_->process(x, n, rsOut_.data());
        src = rsOut_.data();
    }
    buf_.insert(buf_.end(), src, src + m);
    size_t off = 0;
    while (buf_.size() - off >= (size_t)c_.frame) {
        frameReady_(buf_.data() + off);
        off += (size_t)c_.hop;
    }
    if (off) buf_.erase(buf_.begin(), buf_.begin() + (long)off);
    return count_ != before;
}

void SsbOffsetEstimator::frameReady_(const float* f) {
    double e = 0;
    for (int i = 0; i < c_.frame; ++i) e += (double)f[i] * f[i];
    std::memcpy(&blockFrames_[(size_t)nBlock_ * c_.frame], f, sizeof(float) * c_.frame);
    blockEnergy_[nBlock_] = (float)(e / c_.frame);
    ++nBlock_;
    windowSamples_ += c_.hop;
    if (nBlock_ == c_.blockFrames) flushBlock_();
}

/* ★ VOICED-FRAME SELECTION. The rig kept the loudest third of a whole clip; live, the same rule runs on
 *   ~3 s blocks. And a block that is all noise (between overs, a dead channel) is not allowed to lend
 *   its loudest third: a kept frame must also stand 3 dB over the block's quiet frames — noise frames
 *   of 80 ms vary by far less than that, speech by far more. Digital silence (squelch) is never kept.
 * ★★ The QUIETEST fifth is measured too (the coarse stage's noise floor, pq_): the gaps between words show the
 *    receiver's own passband — where its noise starts is where a voice COULD start. */
void SsbOffsetEstimator::flushBlock_() {
    const int n = nBlock_;
    nBlock_ = 0;
    if (n <= 0) return;
    std::vector<float> sorted(blockEnergy_.begin(), blockEnergy_.begin() + n);
    std::sort(sorted.begin(), sorted.end());
    const int keep = std::max(1, (int)std::lround(n * c_.keepFraction));
    const float top = sorted[n - keep];
    const float quiet = sorted[n / 10];
    // ★ The noise floor for the coarse stage is the quietest frames over SECONDS, not this block's: a half-second
    //   block of running speech has no gap in it, and its quietest fifth is speech — which flattered the noise and
    //   squashed the voice-over-noise spectrum to ~12 dB (measured on a 40 m QSO that reads 35 dB over whole gaps).
    //   floorE_ follows a block's 10th percentile down at once and creeps back up ~10 % a second.
    const float blockFloor = sorted[n / 10];
    if (blockFloor >= 1e-9f) {
        const double rise = std::pow(1.1, (double)n * c_.hop / c_.rate);
        floorE_ = floorE_ > 0 ? std::min((double)blockFloor, floorE_ * rise) : blockFloor;
    }
    const float quietTop = (float)(2.0 * floorE_);
    const int before = used_;
    double loud = 0; int nl = 0;
    for (int i = 0; i < n; ++i) {
        const float e = blockEnergy_[i];
        if (c_.positionCue && floorE_ > 0 && e <= quietTop && e >= 1e-9f) quiet_(&blockFrames_[(size_t)i * c_.frame]);
        if (e < top || e < 1e-9f || e < quiet * 2.0f) continue;
        analyse_(&blockFrames_[(size_t)i * c_.frame]);
        loud += e; ++nl;
    }
    // ★ The block's speech-to-gap ratio, weighted by the voiced frames it gave (see SsbOffsetEstimate::snrDb).
    if (used_ > before && quiet > 0) {
        snrSum_ += (used_ - before) * 10.0 * std::log10(loud / nl / quiet);
        snrW_ += used_ - before;
    }
    const float voiced = pendingVoicedSeconds();
    if (voiced >= c_.windowVoicedSec) {
        last_ = finish_(); ++count_;
    } else if (windowSamples_ >= (long long)(c_.maxWindowSec * c_.rate)) {
        if (voiced >= c_.minVoicedSec) { last_ = finish_(); ++count_; }
        else if (c_.memoryVoicedSec <= 0) {
            std::fill(acc_.begin(), acc_.end(), 0.0f); used_ = 0; windowSamples_ = 0; snrSum_ = 0; snrW_ = 0; f0Sum_ = 0; f0N_ = 0;
            weight_ = 0; std::fill(pv_.begin(), pv_.end(), 0.0); std::fill(pq_.begin(), pq_.end(), 0.0); nv_ = nq_ = 0;
        } else { used_ = 0; windowSamples_ = 0; }   // memory: the evidence stays (and decays); only the window restarts
    }
}

void SsbOffsetEstimator::quiet_(const float* f) {
    for (int i = 0; i < c_.frame; ++i) fin_[i] = f[i] * win_[i];
    std::fill(fin_.begin() + c_.frame, fin_.end(), 0.0f);
    fft_->forward(fin_.data(), spec_.data());
    const double res = c_.rate / c_.nfft;
    if (gamma_ < 1) { for (auto& v : pq_) v *= gamma_; nq_ *= gamma_; }
    for (int b = 0; b < kBands; ++b) {
        const int i0 = (int)std::ceil(b * kBandHz / res), i1 = std::min((int)spec_.size(), (int)std::ceil((b + 1) * kBandHz / res));
        double s = 0;
        for (int i = i0; i < i1; ++i) s += (double)std::norm(spec_[i]);
        pq_[b] += s / std::max(1, i1 - i0);
    }
    nq_ += 1;
}

void SsbOffsetEstimator::analyse_(const float* f) {
    const double t0 = nowSec();
    const double res = c_.rate / c_.nfft;
    for (int i = 0; i < c_.frame; ++i) fin_[i] = f[i] * win_[i];
    std::fill(fin_.begin() + c_.frame, fin_.end(), 0.0f);
    fft_->forward(fin_.data(), spec_.data());

    // Bins the comb can reach: harmonics to combHi + dMax, mid-points half a pitch above that.
    const int W = (int)std::lround(179.0 / res) | 1;           // whitening window, ~180 Hz (rig: 61 bins at 2.93 Hz)
    const int need = std::min((int)spec_.size(),
                              (int)((c_.combHiHz + c_.f0Hi) / res) + W + 2);
    logS_.resize(need); white_.resize(need); prefix_.resize(need + 1);
    for (int i = 0; i < need; ++i) {
        const float re = spec_[i].real(), im = spec_[i].imag();
        logS_[i] = 0.5f * std::log(re * re + im * im + 1e-18f);
    }
    prefix_[0] = 0;
    for (int i = 0; i < need; ++i) prefix_[i + 1] = prefix_[i] + logS_[i];
    const int h = W / 2;
    for (int i = 0; i < need; ++i) {
        const int a = std::max(0, i - h), b = std::min(need, i + h + 1);
        white_[i] = logS_[i] - (prefix_[b] - prefix_[a]) / W;    // 'same' convolution: zero beyond the edges
    }
    const float* S = white_.data();

    // ── Pitch candidates ──
    // ★ COARSE: the whitened spectrum's autocorrelation over the comb band peaks at the harmonic
    //   SPACING — which a mistuning does not change. Its three best local maxima are this frame's
    //   plausible pitches; FINE: each is refined ±3 Hz in 0.5 Hz steps by the comb itself.
    std::vector<float> f0s;
    float pitchHz = 0;   // this frame's F0 (see SsbOffsetEstimate::f0Hz); 0 = not measured (fullPitchSearch)
    const int b0 = (int)std::ceil(c_.combLoHz / res), b1 = std::min(need - 1, (int)(c_.combHiHz / res));
    if (c_.fullPitchSearch) {
        for (float f0 = c_.f0Lo; f0 <= c_.f0Hi; f0 += 1.0f) f0s.push_back(f0);
    } else {
        const int l0 = (int)std::floor(c_.f0Lo / res), l1 = (int)std::ceil(c_.f0Hi / res);
        std::vector<float> R(l1 + 2, 0.0f);
        for (int L = l0 - 1; L <= l1 + 1; ++L) {
            if (L <= 0) continue;
            double s = 0;
            for (int i = b0; i + L <= b1; ++i) s += (double)S[i] * S[i + L];
            R[L] = (float)s;
        }
        // three highest local maxima
        int best[3] = {-1, -1, -1};
        for (int L = l0; L <= l1; ++L) {
            if (!(R[L] > R[L - 1] && R[L] >= R[L + 1])) continue;
            for (int k = 0; k < 3; ++k) {
                if (best[k] < 0 || R[L] > R[best[k]]) {
                    for (int q = 2; q > k; --q) best[q] = best[q - 1];
                    best[k] = L; break;
                }
            }
        }
        /* ★ THE PITCH, for SsbOffsetEstimate::f0Hz. A comb of spacing f0 correlates with itself at f0, 2·f0, 3·f0…
         *   almost equally, so the HIGHEST lag is often a multiple (measured on the synthetic voice: a 100 Hz voice
         *   read 280-350 Hz from the winning comb, which fits every third harmonic as well as all of them). The
         *   pitch is the SMALLEST lag that is a local maximum within half of the best — half the spacing is never
         *   one (a shift of half a pitch puts harmonics on the gaps: R goes negative there). */
        if (best[0] >= 0 && R[best[0]] > 0) {
            for (int L = l0; L <= l1; ++L)
                if (R[L] > R[L - 1] && R[L] >= R[L + 1] && R[L] >= 0.5f * R[best[0]]) { pitchHz = (float)(L * res); break; }
        }
        const float span = 0.5f * (c_.pitchSteps - 1) * c_.pitchStepHz;
        for (int k = 0; k < 3; ++k) {
            if (best[k] < 0) continue;
            // sub-bin peak, then the fine search around it
            const int L = best[k];
            const float y0 = R[L - 1], y1 = R[L], y2 = R[L + 1];
            const float den = y0 - 2 * y1 + y2;
            const float frac = den != 0 ? 0.5f * (y0 - y2) / den : 0.0f;
            const float fc = (float)((L + std::max(-0.5f, std::min(0.5f, frac))) * res);
            for (int q = 0; q < c_.pitchSteps; ++q) {
                const float f0 = fc - span + q * c_.pitchStepHz;
                if (f0 >= c_.f0Lo - 3 && f0 <= c_.f0Hi + 3) f0s.push_back(f0);
            }
        }
        if (f0s.empty()) return;
    }

    // ── Pitch-harmonic alignment: for each offset d, the best comb k·f0 + d over the candidate pitches ──
    // ★★ Teeth k ≥ 1 inside [combLo, combHi] score FOR d; teeth k ≤ 0 (at d, d − f0, …) inside [belowCarrierLo, combHi]
    //    score AGAINST it (belowCarrierWeight). A voice has no harmonic at or below its carrier, so an alias a pitch
    //    above the truth — which claims the true first harmonic as its own "zeroth" — is refuted by it. That is the
    //    owner's "super thin" heard by the comb: tuned high, the bottom of the passband is EMPTY.
    const float NEG = -1e9f;
    std::fill(cur_.begin(), cur_.end(), NEG);
    const float lo = c_.combLoHz, hi = c_.combHiHz, d0 = ds_[0], step = c_.dStepHz;
    const float nlo = c_.belowCarrierLoHz, wNeg = c_.belowCarrierWeight;
    const float invRes = (float)(1.0 / res);
    for (float f0 : f0s) {
        std::fill(sum_.begin(), sum_.end(), 0.0f);
        std::fill(cnt_.begin(), cnt_.end(), 0.0f);
        const float half = f0 * 0.5f;
        const int kMax = (int)((hi + c_.dMaxHz) / f0);
        for (int k = 1; k <= kMax; ++k) {
            const float base = k * f0;
            // offsets for which lo < base + d < hi — a contiguous run of j
            int jlo = (int)std::floor((lo - base - d0) / step) + 1;
            int jhi = (int)std::ceil((hi - base - d0) / step) - 1;
            jlo = std::max(jlo, 0); jhi = std::min(jhi, nd_ - 1);
            for (int j = jlo; j <= jhi; ++j) {
                const float p = base + ds_[j];
                const int ib = (int)(p * invRes + 0.5f), im = (int)((p + half) * invRes + 0.5f);
                if (im >= need) continue;
                sum_[j] += S[ib] - S[im];
                cnt_[j] += 1.0f;
            }
        }
        if (wNeg > 0) {
            for (int k = 0; ; --k) {                       // teeth at d + k·f0, k ≤ 0
                const float base = k * f0;
                if (c_.dMaxHz + base < nlo) break;
                int jlo = (int)std::floor((nlo - base - d0) / step) + 1;
                int jhi = (int)std::ceil((hi - base - d0) / step) - 1;
                jlo = std::max(jlo, 0); jhi = std::min(jhi, nd_ - 1);
                for (int j = jlo; j <= jhi; ++j) {
                    const float p = base + ds_[j];
                    const int ib = (int)(p * invRes + 0.5f), im = (int)((p + half) * invRes + 0.5f);
                    if (im >= need) continue;
                    sum_[j] -= wNeg * (S[ib] - S[im]);
                }
            }
        }
        for (int j = 0; j < nd_; ++j)
            if (cnt_[j] >= 4.0f) cur_[j] = std::max(cur_[j], sum_[j] / cnt_[j]);
    }
    float mx = NEG;
    double mean = 0; int nv = 0;
    for (int j = 0; j < nd_; ++j) if (cur_[j] > NEG) { mx = std::max(mx, cur_[j]); mean += cur_[j]; ++nv; }
    if (nv < nd_ / 2 || mx < c_.voicedScore) { cpuSec_ += nowSec() - t0; return; }   // not voiced enough
    mean /= nv;
    double var = 0;
    for (int j = 0; j < nd_; ++j) if (cur_[j] > NEG) var += (cur_[j] - mean) * (cur_[j] - mean);
    const float sd = (float)std::sqrt(var / nv) + 1e-9f;
    // ★ Memory (FAST): what is already known fades by gamma_ per voiced frame before this one is added.
    // ★★ …and the memory belongs to ONE VOICE. In a QSO the other station's evidence is a different comb: a deep voice
    //    (F0 ~100 Hz) on tune leaves its pitch alias at +100 in the memory, and a child 70 Hz off then read +88 —
    //    between the two, right for neither (test-binaural §10). So there are TWO memories, one per voice: when the
    //    voice now speaking is pitched far from the active memory's (kSpeakerRatio), the active one is PARKED and the
    //    parked one comes back if it matches the new voice (a QSO's two stations, alternating); otherwise a fresh one
    //    starts. A single memory cut at every change lost each station's evidence at every over, and a deep voice's comb
    //    — nearly periodic in its pitch — needs a second or two of intonation before its truth stands clear of its
    //    aliases again. A parked memory moves with the dial (shiftAudio) and fades slowly (kParkedFade × the horizon).
    if (pitchHz > 0) f0Recent_ = f0Recent_ > 0 ? 0.85 * f0Recent_ + 0.15 * pitchHz : pitchHz;
    if (gamma_ < 1 && f0N_ > 3 && f0Recent_ > 0) {
        auto far = [&](double f0) { const double r = f0Recent_ > f0 ? f0Recent_ / f0 : f0 / f0Recent_; return r > kSpeakerRatio; };
        if (far(f0Sum_ / f0N_)) {
            const bool back = park_.f0N > 3 && !far(park_.f0Sum / park_.f0N);
            Memory cur{acc_, pv_, nv_, weight_, f0Sum_, f0N_, snrSum_, snrW_, framesTotal_};
            if (back) {
                acc_.swap(park_.acc); pv_.swap(park_.pv);
                nv_ = park_.nv; weight_ = park_.weight; f0Sum_ = park_.f0Sum; f0N_ = park_.f0N;
                snrSum_ = park_.snrSum; snrW_ = park_.snrW; framesTotal_ = park_.frames;
            } else {
                std::fill(acc_.begin(), acc_.end(), 0.0f); std::fill(pv_.begin(), pv_.end(), 0.0);
                nv_ = weight_ = snrSum_ = snrW_ = 0; framesTotal_ = 0;
                f0Sum_ = f0Recent_; f0N_ = 1;
            }
            park_ = std::move(cur);
            ++speakerChanges_;
        }
    }
    if (gamma_ < 1) {
        const double g = gamma_;
        for (auto& v : acc_) v *= (float)g;
        weight_ *= g; snrSum_ *= g; snrW_ *= g; f0Sum_ *= g; f0N_ *= g;
        for (auto& v : pv_) v *= g;
        nv_ *= g;
        if (park_.weight > 0) {
            const double gp = gammaPark_;
            for (auto& v : park_.acc) v *= (float)gp;
            for (auto& v : park_.pv) v *= gp;
            park_.nv *= gp; park_.weight *= gp; park_.f0Sum *= gp; park_.f0N *= gp; park_.snrSum *= gp; park_.snrW *= gp;
        }
    }
    for (int j = 0; j < nd_; ++j) acc_[j] += cur_[j] > NEG ? (float)((cur_[j] - mean) / sd) : 0.0f;
    weight_ += 1;
    ++used_; ++framesTotal_;
    if (pitchHz > 0) { f0Sum_ += pitchHz; f0N_ += 1; }
    // ★ The coarse stage's voiced spectrum: this frame's power in kBandHz bands (spec_ still holds its FFT).
    if (c_.positionCue) {
        for (int b = 0; b < kBands; ++b) {
            const int i0 = (int)std::ceil(b * kBandHz / res), i1 = std::min((int)spec_.size(), (int)std::ceil((b + 1) * kBandHz / res));
            double s = 0;
            for (int i = i0; i < i1; ++i) s += (double)std::norm(spec_[i]);
            pv_[b] += s / std::max(1, i1 - i0);
        }
        nv_ += 1;
    }
    cpuSec_ += nowSec() - t0;
}

/* ★★ THE COARSE STAGE — where the voice's energy sits (the owner, 2026-10-17: on LSB "tuning below makes it deep and a
 *    bit robot sounding, above makes it super thin"). Mistuning slides the whole voice: tuned high there is an empty gap
 *    at the bottom of the passband, tuned low the bass is pushed against the floor. Measured from the audio alone, so
 *    it works on every backend:
 *    • the receiver's passband floor — the lowest band where the noise between words comes within 10 dB of its in-band
 *      median (a voice cannot start below the filter that carries it);
 *    • the voice's lower edge — the lowest band, from there up, where voiced frames stand clear of that noise (at least
 *      6 dB, and within 20 dB of the voice's own best band) for three bands running.
 *    A transmitter cuts its own low end anywhere from ~60 Hz (ESSB) to ~400 Hz, so the edge places the carrier only to
 *    [edge − 400 − tol, edge − 60 + tol]. On the deep side the edge is hidden below the floor: then all it says is
 *    "at or below edge − 60 + tol" — open-ended. Wide, but it never confuses deep with thin. */
void SsbOffsetEstimator::coarse_(SsbOffsetEstimate& e) const {
    if (!c_.positionCue || nv_ < 1 || nq_ < 1) return;
    std::vector<double> v(kBands), q(kBands);
    for (int b = 0; b < kBands; ++b) { v[b] = pv_[b] / nv_; q[b] = pq_[b] / nq_; }
    const int m0 = (int)(300 / kBandHz), m1 = (int)(2400 / kBandHz);
    std::vector<double> mid(q.begin() + m0, q.begin() + m1);
    std::nth_element(mid.begin(), mid.begin() + mid.size() / 2, mid.end());
    const double medQ = mid[mid.size() / 2];
    if (!(medQ > 1e-14)) return;                       // no noise between words to read the filter from
    int rx = 0;
    while (rx < m0 && q[rx] < 0.1 * medQ) ++rx;
    const int top = (int)(c_.combHiHz / kBandHz);
    std::vector<float> snr(kBands, -99.0f);
    for (int b = 1; b + 1 < kBands; ++b) {
        const double vv = v[b - 1] + v[b] + v[b + 1], qq = std::max(q[b - 1] + q[b] + q[b + 1], 3e-3 * medQ);
        snr[b] = (float)(10.0 * std::log10(vv / qq + 1e-12));
    }
    float peak = -99.0f;
    for (int b = std::max(rx, 1); b <= top; ++b) peak = std::max(peak, snr[b]);
    if (peak < kCoarseMinPeakDb) return;               // the voice never stands clear of the noise: no position
    const float thr = std::max(kCoarseEdgeDb, peak - kCoarseBelowPeakDb);
    int lo = -1;
    for (int b = std::max(rx, 1); b + 2 <= top; ++b)
        if (snr[b] >= thr && snr[b + 1] >= thr && snr[b + 2] >= thr) { lo = b; break; }
    if (lo < 0) return;
    e.passbandLowHz = rx * kBandHz;
    e.voiceLowHz = lo * kBandHz;
    const bool pinned = e.voiceLowHz <= e.passbandLowHz + kCoarseTolHz;
    e.coarseHiHz = e.voiceLowHz + kCoarseTolHz - kTxLowCutMinHz;
    e.coarseLoHz = pinned ? -c_.dMaxHz : e.voiceLowHz - kCoarseTolHz - kTxLowCutMaxHz;
}

/* ★★ THE SHARP PEAK, AND HOW SURE. The summed curve is high-passed (minus its own ±80 Hz moving
 *    average, edges padded — the rig's 81-point filter at 2 Hz steps): aliases are broad because they
 *    move with the pitch, the true offset is sharp because it does not.
 * ★★ TWO STAGES (2026-10-17). The comb fixes the offset FINELY but only modulo the pitch on its own (k·f0 + d fits
 *    d ± f0 nearly as well); the coarse stage says roughly where the voice sits. The answer is the comb's strongest
 *    peak INSIDE the coarse interval. If the comb's strongest peak anywhere lies outside it by a clear margin, the
 *    two cannot be reconciled and nothing is believed (reconciled = false).
 *    CONFIDENCE = the winner's height / the frames behind it (how firmly every voiced frame agrees on it);
 *    CLARITY = (winner − the best rival more than 30 Hz away and inside the interval) / the curve's spread. */
SsbOffsetEstimate SsbOffsetEstimator::finish_() {
    SsbOffsetEstimate e;
    const int used = used_;
    const bool memory = c_.memoryVoicedSec > 0;
    e.frames = memory ? std::min(framesTotal_, (int)std::lround(weight_)) : used;
    e.minFrames = c_.minFrames;
    e.gate = c_.gate;
    e.voicedSeconds = (float)(e.frames * (double)c_.hop / c_.rate);
    if (weight_ > 0) {
        const int hw = std::max(1, (int)std::lround(80.0f / c_.dStepHz));
        auto highpass = [&](const std::vector<float>& a, double per) {   // ±hw moving average removed, per frame
            std::vector<float> o(nd_);
            double s = 0;
            for (int q = -hw; q <= hw; ++q) s += a[std::min(nd_ - 1, std::max(0, q))];
            for (int j = 0; j < nd_; ++j) {
                o[j] = (float)((a[j] - s / (2 * hw + 1)) / per);
                s += a[std::min(nd_ - 1, j + hw + 1)] - a[std::max(0, j - hw)];
            }
            return o;
        };
        const std::vector<float> hp = highpass(acc_, weight_);
        coarse_(e);
        const bool haveCoarse = e.coarseHiHz != 0.0f || e.coarseLoHz != 0.0f;
        auto inside = [&](int j) { return !haveCoarse || (ds_[j] >= e.coarseLoHz && ds_[j] <= e.coarseHiHz); };
        int jg = 0, jb = -1;
        for (int j = 1; j < nd_; ++j) if (hp[j] > hp[jg]) jg = j;
        for (int j = 0; j < nd_; ++j) if (inside(j) && (jb < 0 || hp[j] > hp[jb])) jb = j;
        double m = 0, v = 0;
        for (float y : hp) m += y;
        m /= nd_;
        for (float y : hp) v += (y - m) * (y - m);
        const float sd = (float)std::sqrt(v / nd_) + 1e-9f;
        if (jb < 0) { jb = jg; e.reconciled = false; }
        else if (jb != jg && (hp[jg] - hp[jb]) > kReconcileSd * sd) e.reconciled = false;
        float frac = 0;
        if (jb > 0 && jb < nd_ - 1) {
            const float y0 = hp[jb - 1], y1 = hp[jb], y2 = hp[jb + 1], den = y0 - 2 * y1 + y2;
            if (den < 0) frac = std::max(-0.5f, std::min(0.5f, 0.5f * (y0 - y2) / den));
        }
        e.offsetHz = ds_[jb] + frac * c_.dStepHz;
        float rival = -1e30f;
        const int ex = (int)std::lround(30.0f / c_.dStepHz);
        for (int j = 0; j < nd_; ++j) if (std::abs(j - jb) > ex && inside(j)) rival = std::max(rival, hp[j]);
        e.clarity = rival > -1e29f ? (hp[jb] - rival) / sd : 10.0f;
        e.snrDb = snrW_ > 0 ? (float)(snrSum_ / snrW_) : 0.0f;
        e.f0Hz = f0N_ > 0 ? (float)(f0Sum_ / f0N_) : 0.0f;
        e.confidence = hp[jb];   // already per frame
        e.valid = true;
    }
    used_ = 0; windowSamples_ = 0;
    if (!memory) {
        std::fill(acc_.begin(), acc_.end(), 0.0f);
        snrSum_ = 0; snrW_ = 0; f0Sum_ = 0; f0N_ = 0; weight_ = 0;
        std::fill(pv_.begin(), pv_.end(), 0.0); std::fill(pq_.begin(), pq_.end(), 0.0); nv_ = nq_ = 0;
    }
    return e;
}

SsbOffsetEstimate SsbOffsetEstimator::estimate(const float* x, int n, double inRate, Profile p) {
    return estimate(x, n, inRate, Config::forProfile(p));
}

SsbOffsetEstimate SsbOffsetEstimator::estimate(const float* x, int n, double inRate, Config c) {
    c.windowVoicedSec = 1e9f; c.maxWindowSec = 1e9f;   // one window: the whole buffer
    c.memoryVoicedSec = 0;                              // …all of it counted, none of it faded
    SsbOffsetEstimator est(inRate, c);
    est.push(x, n);
    est.flushBlock_();
    return est.finish_();
}

// ── SsbShifter ────────────────────────────────────────────────────────────────────────────────────

SsbShifter::SsbShifter(double rate, int taps) : rate_(rate), K_(taps | 1) {
    const int m = (K_ - 1) / 2;
    h_.assign(K_, 0.0f);
    for (int i = 0; i < K_; ++i) {
        const int k = i - m;
        if (k % 2 == 0) continue;          // the ideal Hilbert transformer is zero on even taps
        const double t = 2.0 * kPi * i / (K_ - 1);
        const double w = 0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t);   // Blackman
        h_[i] = (float)(2.0 / (kPi * k) * w);
    }
    // y = Σ h[i]·in[n−i]; over the window w (newest at w[K−1]) that is Σ h[K−1−k]·w[k]. Keep the non-zero half.
    for (int k = 0; k < K_; ++k)
        if (h_[K_ - 1 - k] != 0.0f) { hk_.push_back(k); hc_.push_back(h_[K_ - 1 - k]); }
    hist_.assign(K_ - 1, 0.0f);
}

void SsbShifter::setShift(double hz) { shiftHz_ = hz; dph_ = 2.0 * kPi * hz / rate_; }

void SsbShifter::reset() { std::fill(hist_.begin(), hist_.end(), 0.0f); hist_.resize(K_ - 1); ph_ = 0; }

void SsbShifter::process(float* x, int n) {
    const int H = K_ - 1, m = H / 2;
    hist_.resize((size_t)H + n);
    std::memcpy(&hist_[H], x, sizeof(float) * n);
    const float* b = hist_.data();
    if (dph_ == 0.0 && ph_ == 0.0) {
        // ★ No correction: the delayed input, so engaging the shift later never jumps in time.
        for (int i = 0; i < n; ++i) x[i] = b[i + m];
    } else {
        for (int i = 0; i < n; ++i) {
            // y[i] = sum h[k]·in[i−k], causal: window b[i .. i+K−1], newest at b[i+K−1]
            const float* w = b + i;
            float q = 0;
            for (size_t t = 0; t < hk_.size(); ++t) q += hc_[t] * w[hk_[t]];   // the non-zero taps only
            const float re = w[m];
            x[i] = (float)(re * std::cos(ph_) - q * std::sin(ph_));
            ph_ += dph_;
            if (ph_ > kPi) ph_ -= 2.0 * kPi; else if (ph_ < -kPi) ph_ += 2.0 * kPi;
        }
    }
    std::memmove(hist_.data(), hist_.data() + n, sizeof(float) * H);
    hist_.resize(H);
}

// ── SsbAutoTune ───────────────────────────────────────────────────────────────────────────────────

SsbAutoTune::SsbAutoTune(double rate)
    : est_(std::make_unique<SsbOffsetEstimator>(rate, SsbOffsetEstimator::Profile::Slow)), shift_(rate) {}
SsbAutoTune::~SsbAutoTune() = default;

void SsbAutoTune::setSideband(int sb) {
    sb = sb > 0 ? 1 : sb < 0 ? -1 : 0;
    if (sb == sb_) return;
    sb_ = sb;
    reset();
}

void SsbAutoTune::reset() {
    est_->reset();
    havePrev_ = false; prev_ = 0;
    applied_ = 0;
    shift_.setShift(0);
    shift_.reset();
}

void SsbAutoTune::process(float* x, int n) {
    if (sb_ != 0 && est_->push(x, n)) {
        const SsbOffsetEstimate& e = est_->last();
        const bool ok = e.passes() && std::fabs(e.offsetHz) <= kMaxCorrectHz;
        if (!ok) {
            havePrev_ = false;                  // the chain of agreeing windows is broken; the shift stays
        } else {
            if (havePrev_ && std::fabs(e.offsetHz - prev_) <= kAgreeHz) {
                const float d = 0.5f * (e.offsetHz + prev_);
                const float want = std::fabs(d) >= kDeadbandHz ? -d : 0.0f;
                if (std::fabs(want - applied_) > kRefineHz) {   // a refinement of a few Hz is not worth a change
                    applied_ = want;
                    shift_.setShift(applied_);
                    ++corrections_;
                }
            }
            prev_ = e.offsetHz; havePrev_ = true;
        }
    }
    shift_.process(x, n);
}

// ── RecogniserFeed ────────────────────────────────────────────────────────────────────────────────

RecogniserFeed::RecogniserFeed() : tune_(kOutRate) {}
RecogniserFeed::~RecogniserFeed() = default;

void RecogniserFeed::requestTuning(int sideband, double dialHz) {
    reqSb_.store(sideband, std::memory_order_relaxed);
    reqDial_.store((long long)std::llround(dialHz), std::memory_order_relaxed);
    reqGen_.fetch_add(1, std::memory_order_release);
}

int RecogniserFeed::process(const float* in, int n, int inRate, float* out) {
    const int g = reqGen_.load(std::memory_order_acquire);
    if (g != appliedGen_) {
        appliedGen_ = g;
        // ★ Any retune or mode change: the old offset belongs to another signal. Reset, then arm.
        tune_.setSideband(0);
        tune_.setSideband(reqSb_.load(std::memory_order_relaxed));
    }
    if (inRate <= 0 || n <= 0) return 0;
    int m;
    if (inRate == kOutRate) {
        std::memcpy(out, in, sizeof(float) * n);
        m = n;
    } else {
        if (!rs_ || inRate != rate_) { rs_ = std::make_unique<RationalResampler>(inRate, kOutRate); rate_ = inRate; }
        m = rs_->process(in, n, out);
    }
    tune_.process(out, m);
    double s = 0;
    for (int i = 0; i < m; ++i) s += (double)out[i] * out[i];
    rms_ = m > 0 ? (float)std::sqrt(s / m) : 0.0f;
    return m;
}

}  // namespace vibedsp

// ── C API ─────────────────────────────────────────────────────────────────────────────────────────
using vibedsp::RecogniserFeed;

extern "C" {
void* vibe_rxfeed_create(void) { return new RecogniserFeed(); }
void  vibe_rxfeed_destroy(void* h) { delete static_cast<RecogniserFeed*>(h); }
void  vibe_rxfeed_set_tuning(void* h, int sideband, double dialHz) {
    if (h) static_cast<RecogniserFeed*>(h)->requestTuning(sideband, dialHz);
}
int vibe_rxfeed_process(void* h, const float* in, int n, int inRate, float* out, int outCap) {
    if (!h || !in || !out || n <= 0 || inRate <= 0) return 0;
    if (outCap < RecogniserFeed::maxOut(n, inRate)) return 0;
    return static_cast<RecogniserFeed*>(h)->process(in, n, inRate, out);
}
float vibe_rxfeed_dial_correction(void* h) {
    if (!h) return 0;
    auto& t = static_cast<RecogniserFeed*>(h)->tune();
    // audio shift −d: USB (audio = rf − dial) wants the dial moved +d; LSB (audio = dial − rf) −d.
    return t.sideband() > 0 ? -t.appliedHz() : t.sideband() < 0 ? t.appliedHz() : 0.0f;
}
float vibe_rxfeed_rms(void* h) { return h ? static_cast<RecogniserFeed*>(h)->lastRms() : 0.0f; }
int vibe_rxfeed_stats(void* h, float* o) {
    if (!h || !o) return 0;
    auto& t = static_cast<RecogniserFeed*>(h)->tune();
    const auto& e = t.lastEstimate();
    o[0] = e.offsetHz; o[1] = e.confidence; o[2] = e.voicedSeconds; o[3] = e.passes() ? 1.0f : 0.0f;
    o[4] = (float)t.estimates(); o[5] = (float)t.corrections(); o[6] = t.appliedHz();
    return 7;
}
}
