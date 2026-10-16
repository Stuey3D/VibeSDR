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

/* ★★ THE GATE, MEASURED (vibeserver/test-ssb-offset-clips.cpp, real UberSDR clips, 2026-10-16).
 *    Two rules, both from the data:
 *    1. STRENGTH rises with the offset. Correct 10 s windows score 0.8-1.3 (mean per-frame z at the
 *       peak); noise and two-station muddles 0.1-0.6. A pitch alias lies a pitch away from the truth, so
 *       large offsets are where wrong answers live: gate = 0.55 + 0.001/Hz (0.75 at ±200 Hz, which a real
 *       200 Hz mistune of a strong voice clears at 0.78-1.15).
 *    2. A LARGE OFFSET NEEDS A CLEAN SIGNAL. Every believed-looking wrong answer over ±40 Hz came from a
 *       window at ≤10 dB speech-to-gap: the research's +162 Hz on 3648.2 kHz (8-9 dB), VOLMET buried in
 *       noise reading +194/+202 Hz (4-6 dB), the other station of a QSO (10 dB). Every correct one was at
 *       17-22 dB. So beyond ±40 Hz a window must also be at 12 dB or better. Near zero no SNR is asked:
 *       a small correction of a weak signal is harmless, and the auto-tune leaves ±10 Hz alone anyway. */
const float SsbOffsetEstimator::kGateBase = 0.55f;
const float SsbOffsetEstimator::kGatePerHz = 0.001f;
const float SsbOffsetEstimator::kLargeHz = 40.0f;
const float SsbOffsetEstimator::kLargeMinSnrDb = 12.0f;
float SsbOffsetEstimator::gateFor(float offsetHz) { return kGateBase + kGatePerHz * std::fabs(offsetHz); }

bool SsbOffsetEstimate::passes() const {
    using E = SsbOffsetEstimator;
    return valid && frames >= minFrames && confidence >= E::gateFor(offsetHz)
        && (std::fabs(offsetHz) <= E::kLargeHz || snrDb >= E::kLargeMinSnrDb);
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
        c.minFrames = 20;                // 0.4 s of 40 ms frames at a 20 ms step
    }
    return c;
}

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
    blockFrames_.resize((size_t)c_.blockFrames * c_.frame);
    blockEnergy_.resize(c_.blockFrames);
}

SsbOffsetEstimator::~SsbOffsetEstimator() = default;

void SsbOffsetEstimator::reset() {
    if (rs_) rs_->reset();
    buf_.clear(); bufFill_ = 0; nBlock_ = 0;
    std::fill(acc_.begin(), acc_.end(), 0.0f);
    used_ = 0; windowSamples_ = 0; snrSum_ = 0;
    last_ = SsbOffsetEstimate{};
    count_ = 0;
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
 *   of 80 ms vary by far less than that, speech by far more. Digital silence (squelch) is never kept. */
void SsbOffsetEstimator::flushBlock_() {
    const int n = nBlock_;
    nBlock_ = 0;
    if (n <= 0) return;
    std::vector<float> sorted(blockEnergy_.begin(), blockEnergy_.begin() + n);
    std::sort(sorted.begin(), sorted.end());
    const int keep = std::max(1, (int)std::lround(n * c_.keepFraction));
    const float top = sorted[n - keep];
    const float quiet = sorted[n / 10];
    const int before = used_;
    double loud = 0; int nl = 0;
    for (int i = 0; i < n; ++i) {
        const float e = blockEnergy_[i];
        if (e < top || e < 1e-9f || e < quiet * 2.0f) continue;
        analyse_(&blockFrames_[(size_t)i * c_.frame]);
        loud += e; ++nl;
    }
    // ★ The block's speech-to-gap ratio, weighted by the voiced frames it gave (see SsbOffsetEstimate::snrDb).
    if (used_ > before && quiet > 0) snrSum_ += (used_ - before) * 10.0 * std::log10(loud / nl / quiet);
    const float voiced = pendingVoicedSeconds();
    if (voiced >= c_.windowVoicedSec) {
        last_ = finish_(); ++count_;
    } else if (windowSamples_ >= (long long)(c_.maxWindowSec * c_.rate)) {
        if (voiced >= c_.minVoicedSec) { last_ = finish_(); ++count_; }
        else { std::fill(acc_.begin(), acc_.end(), 0.0f); used_ = 0; windowSamples_ = 0; snrSum_ = 0; }
    }
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
                              (int)((c_.combHiHz + c_.dMaxHz + c_.f0Hi) / res) + W + 2);
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
        for (int k = 0; k < 3; ++k) {
            if (best[k] < 0) continue;
            // sub-bin peak, then the fine search around it
            const int L = best[k];
            const float y0 = R[L - 1], y1 = R[L], y2 = R[L + 1];
            const float den = y0 - 2 * y1 + y2;
            const float frac = den != 0 ? 0.5f * (y0 - y2) / den : 0.0f;
            const float fc = (float)((L + std::max(-0.5f, std::min(0.5f, frac))) * res);
            for (float f0 = fc - 3.0f; f0 <= fc + 3.0f + 1e-3f; f0 += 0.5f)
                if (f0 >= c_.f0Lo - 3 && f0 <= c_.f0Hi + 3) f0s.push_back(f0);
        }
        if (f0s.empty()) return;
    }

    // ── Pitch-harmonic alignment: for each offset d, the best comb k·f0 + d over the candidate pitches ──
    const float NEG = -1e9f;
    std::fill(cur_.begin(), cur_.end(), NEG);
    const float lo = c_.combLoHz, hi = c_.combHiHz, d0 = ds_[0], step = c_.dStepHz;
    const float invRes = (float)(1.0 / res);
    for (float f0 : f0s) {
        std::fill(sum_.begin(), sum_.end(), 0.0f);
        std::fill(cnt_.begin(), cnt_.end(), 0.0f);
        const int kMax = (int)((hi + c_.dMaxHz) / f0);
        const float half = f0 * 0.5f;
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
    for (int j = 0; j < nd_; ++j) acc_[j] += cur_[j] > NEG ? (float)((cur_[j] - mean) / sd) : 0.0f;
    ++used_;
    cpuSec_ += nowSec() - t0;
}

/* ★★ THE SHARP PEAK, AND HOW SURE. The summed curve is high-passed (minus its own ±80 Hz moving
 *    average, edges padded — the rig's 81-point filter at 2 Hz steps): aliases are broad because they
 *    move with the pitch, the true offset is sharp because it does not.
 *    CONFIDENCE = (peak − the best value more than 30 Hz away) / the curve's standard deviation. A clean
 *    voice stands alone; two stations at different offsets, or a weak signal whose "peak" is one noise
 *    ripple among many, have a rival close behind and score low. */
SsbOffsetEstimate SsbOffsetEstimator::finish_() {
    SsbOffsetEstimate e;
    const int used = used_;
    e.frames = used;
    e.minFrames = c_.minFrames;
    e.voicedSeconds = (float)(used * (double)c_.hop / c_.rate);
    if (used > 0) {
        const int hw = std::max(1, (int)std::lround(80.0f / c_.dStepHz));
        std::vector<float> hp(nd_);
        for (int j = 0; j < nd_; ++j) {
            double s = 0;
            for (int q = -hw; q <= hw; ++q) s += acc_[std::min(nd_ - 1, std::max(0, j + q))];
            hp[j] = acc_[j] - (float)(s / (2 * hw + 1));
        }
        int jb = 0;
        for (int j = 1; j < nd_; ++j) if (hp[j] > hp[jb]) jb = j;
        float frac = 0;
        if (jb > 0 && jb < nd_ - 1) {
            const float y0 = hp[jb - 1], y1 = hp[jb], y2 = hp[jb + 1], den = y0 - 2 * y1 + y2;
            if (den < 0) frac = std::max(-0.5f, std::min(0.5f, 0.5f * (y0 - y2) / den));
        }
        e.offsetHz = ds_[jb] + frac * c_.dStepHz;
        double m = 0, v = 0;
        for (float y : hp) m += y;
        m /= nd_;
        for (float y : hp) v += (y - m) * (y - m);
        const float sd = (float)std::sqrt(v / nd_) + 1e-9f;
        float rival = -1e30f;
        const int ex = (int)std::lround(30.0f / c_.dStepHz);
        for (int j = 0; j < nd_; ++j) if (std::abs(j - jb) > ex) rival = std::max(rival, hp[j]);
        e.clarity = (hp[jb] - rival) / sd;
        e.snrDb = (float)(snrSum_ / used);
        e.confidence = hp[jb] / used;
        e.valid = true;
    }
    std::fill(acc_.begin(), acc_.end(), 0.0f);
    used_ = 0; windowSamples_ = 0; snrSum_ = 0;
    return e;
}

SsbOffsetEstimate SsbOffsetEstimator::estimate(const float* x, int n, double inRate, Profile p) {
    Config c = Config::forProfile(p);
    c.windowVoicedSec = 1e9f; c.maxWindowSec = 1e9f;   // one window: the whole buffer
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
