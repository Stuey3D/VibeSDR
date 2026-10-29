// decoders/drm/drm_receiver.cpp — see drm_receiver.h for the chain and the reasons.
#include "drm_receiver.h"
#include "vibedsp.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <string>

namespace vibedrm {

namespace {
constexpr int kAcqLen = 19200;          // 0.8 s at 24 kHz: ≥ 30 symbols of every mode
constexpr double kSearchSpanHz = 1500;  // integer frequency search, ±
constexpr float kPi = 3.14159265358979f;

/** ★ The equaliser: Y = X·conj(H) / |H|², W = |H|². NEON / SSE2 four carriers at a time (two complex per register pair). */
void equaliseCells(const cf* X, const cf* H, cf* Y, float* W, int n) {
    int i = 0;
#if VIBE_DRM_NEON
    for (; i + 4 <= n; i += 4) {
        const float32x4x2_t x = vld2q_f32(reinterpret_cast<const float*>(X + i));
        const float32x4x2_t h = vld2q_f32(reinterpret_cast<const float*>(H + i));
        const float32x4_t p = vmlaq_f32(vmulq_f32(h.val[0], h.val[0]), h.val[1], h.val[1]);
        const float32x4_t inv = vdivq_f32(vdupq_n_f32(1.0f), vmaxq_f32(p, vdupq_n_f32(1e-20f)));
        float32x4x2_t y;
        y.val[0] = vmulq_f32(vmlaq_f32(vmulq_f32(x.val[0], h.val[0]), x.val[1], h.val[1]), inv);
        y.val[1] = vmulq_f32(vmlsq_f32(vmulq_f32(x.val[1], h.val[0]), x.val[0], h.val[1]), inv);
        vst2q_f32(reinterpret_cast<float*>(Y + i), y);
        vst1q_f32(W + i, p);
    }
#elif VIBE_DRM_SSE
    for (; i + 2 <= n; i += 2) {
        // ★ SSE2 has no de-interleaving load; two complex per register, the pair arithmetic by shuffles.
        const __m128 x = _mm_loadu_ps(reinterpret_cast<const float*>(X + i));   // xr0 xi0 xr1 xi1
        const __m128 h = _mm_loadu_ps(reinterpret_cast<const float*>(H + i));   // hr0 hi0 hr1 hi1
        const __m128 hh = _mm_mul_ps(h, h);
        const __m128 p = _mm_add_ps(hh, _mm_shuffle_ps(hh, hh, _MM_SHUFFLE(2, 3, 0, 1)));   // |h|² in both lanes
        const __m128 hs = _mm_shuffle_ps(h, h, _MM_SHUFFLE(2, 3, 0, 1));                   // hi0 hr0 hi1 hr1
        const __m128 a = _mm_mul_ps(x, h);    // xr·hr, xi·hi, …
        const __m128 b = _mm_mul_ps(x, hs);   // xr·hi, xi·hr, …
        const __m128 re = _mm_add_ps(a, _mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 3, 0, 1)));   // xr·hr + xi·hi in lanes 0,2
        const __m128 im = _mm_sub_ps(_mm_shuffle_ps(b, b, _MM_SHUFFLE(2, 3, 0, 1)), b);   // xi·hr − xr·hi in lanes 0,2
        const __m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), _mm_max_ps(p, _mm_set1_ps(1e-20f)));
        const __m128 reS = _mm_mul_ps(re, inv), imS = _mm_mul_ps(im, inv);
        // interleave re (lanes 0,2) and im (lanes 0,2) → re0 im0 re1 im1
        const __m128 lo = _mm_unpacklo_ps(reS, imS);   // re0 im0 re? im?
        const __m128 hi = _mm_unpackhi_ps(reS, imS);   // re1 im1 …
        _mm_storeu_ps(reinterpret_cast<float*>(Y + i), _mm_movelh_ps(lo, hi));
        float pw[4]; _mm_storeu_ps(pw, p);
        W[i] = pw[0]; W[i + 1] = pw[2];
    }
#endif
    for (; i < n; ++i) {
        const float p = std::norm(H[i]);
        Y[i] = X[i] * std::conj(H[i]) / std::max(p, 1e-20f);
        W[i] = p;
    }
}

inline cf nearestQam(cf y, int levels) {
    const float a = qamScale(levels);
    const int m = (1 << levels) - 1;
    auto ax = [&](float v) {
        // amplitudes (m − 2r)·a, r = 0..m
        float r = std::round((float(m) - v / a) / 2.0f);
        r = std::min(std::max(r, 0.0f), float(m));
        return (float(m) - 2.0f * r) * a;
    };
    return cf(ax(y.real()), ax(y.imag()));
}
}  // namespace

Receiver::Receiver() { reset(); }
Receiver::~Receiver() = default;

void Receiver::reset() {
    rsRate_ = 0; rsH_.clear(); rsHist_.clear(); rsF_.clear(); rsPos_ = 0;
    buf_.clear(); bufBase_ = 0; samplesIn24_ = 0;
    blankFrom_ = 0; pAvg_ = 0; pSlow_ = 0; blankHold_ = 0; blankRun_ = 0; blanked_ = 0; blankWarm_ = 0;
    blankRecent_[0] = blankRecent_[1] = 0; blankRecentN_[0] = blankRecentN_[1] = 0; blankBucket_ = 0;
    loseSync("reset");
    st_ = RxStatus();
    text_ = TextMessageDecoder();
}

void Receiver::loseSync(const char* why) {
    if (getenv("VIBE_DRM_DEBUG") && state_ != State::Search)
        fprintf(stderr, "[drm] lose sync: %s at %.2f s (locked %d, FAC %lld/%lld, MER %.1f dB)\n", why, double(samplesIn24_) / kFs,
                int(st_.locked), (long long)st_.facGood, (long long)(st_.facGood + st_.facBad), double(facMerAcc_));
    // ★★ THE SIGNAL PANE'S LOCK GROUP: a LOCK that is lost is a re-acquisition, and the clock for the time-to-lock row
    //    restarts from it. A search that fails before it ever locked (the frequency or frame test) is part of the SAME
    //    acquisition — the clock runs on, the lights it had lit go out.
    const bool wasLocked = st_.locked;
    if (wasLocked) { ++st_.reacquisitions; st_.lastLoss = why; st_.lastLossAtS = double(samplesIn24_) / kFs; }
    if (wasLocked || std::string(why) == "reset") acqStart_ = samplesIn24_;
    for (double& t : st_.lockS) t = -1;
    dsPdp_.clear(); dsN_ = 0; dop1_ = dop2_ = cf(0, 0); dopN_ = 0; sdcMerAcc_ = 0; sdcMerN_ = 0;
    st_.delayMs = -1; st_.dopplerHz = -1; st_.sdcMerDb = 0; st_.codeRateA = 0; st_.sdcRate = 0;
    state_ = State::Search;
    mode_ = Mode::None; fft_.reset();
    freq_ = 0; ncoPhase_ = 0; ncoIdx_ = 0;
    havePrev_ = false; prevStep_ = 0; tauAvg_ = 0; tauN_ = 0; stepSum_ = 0; pendingStep_ = 0;
    tScore_.clear(); symCount_ = 0; frameSynced_ = false; sIdx_ = 0; gIdx_ = 0; fsHist_.clear();
    ring_.clear(); frS_ = 0; frY_.clear(); frW_.clear();
    // ★★ …and the averaged delay profile that steers the timing (2026-10-26). It was kept, so a re-acquisition — whose
    //    search places the window afresh — was then steered by a profile measured against the OLD window, and walked
    //    straight back out of alignment: one loss of sync became three in a row (the flaky test-server-drm's replay).
    pdp_.clear(); pdpN_ = 0;
    for (auto& f : sf_) f.have = false;
    lastFrameIdx_ = -1; facFailRun_ = 0; facGoodRun_ = 0; haveFac_ = false;
    merInit_ = false;
    const Sdc keepSdc;   // ★ a lost signal forgets its station: nothing stale survives a resync
    st_.timeSync = st_.freqSync = st_.frameSync = st_.facOk = st_.sdcOk = st_.mscOk = st_.locked = false;
    st_.mode = Mode::None; st_.so = -1; st_.sdc = keepSdc; st_.text.clear(); st_.bitrateBps = 0;
    text_ = TextMessageDecoder();
}

void Receiver::markLock(int stage) {
    if (stage >= 0 && stage < 6 && st_.lockS[stage] < 0) st_.lockS[stage] = double(samplesIn24_ - acqStart_) / kFs;
}

// ── 0. Resampler ─────────────────────────────────────────────────────────────────────────────────────────────────────

void Receiver::resampleIn(const cf* x, int n, double rate) {
    if (rate != rsRate_) {
        rsRate_ = rate; rsHist_.clear(); rsF_.clear(); rsPos_ = 0; rsH_.clear();
        if (rate > kFs * 1.01) {
            // ★ windowed-sinc low-pass, cutoff 8.5 kHz: passes ±4.75 kHz of signal plus the ±1.5 kHz tuning search,
            //   stops above 12 kHz (the 24 kHz output's edge) by the window's ~-70 dB.
            const double fc = 8500.0 / rate, trans = 3500.0 / rate;
            int taps = int(std::ceil(3.6 / trans)) | 1;
            taps = std::min(std::max(taps, 15), 255);
            rsH_.resize(size_t(taps));
            double sum = 0;
            for (int i = 0; i < taps; ++i) {
                const double t = i - (taps - 1) / 2.0;
                const double s = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
                const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
                rsH_[size_t(i)] = float(s * w); sum += s * w;
            }
            for (float& h : rsH_) h = float(h / sum);
        }
    }
    if (std::abs(rate - kFs) < 0.5) {
        buf_.insert(buf_.end(), x, x + n); samplesIn24_ += n; return;
    }
    // filter at the input rate
    const size_t T = rsH_.size();
    if (T) {
        rsHist_.insert(rsHist_.end(), x, x + n);
        const size_t avail = rsHist_.size() >= T ? rsHist_.size() - T + 1 : 0;
        for (size_t i = 0; i < avail; ++i) {
            float re = 0, im = 0;
            const cf* p = rsHist_.data() + i;
            for (size_t j = 0; j < T; ++j) { re += p[j].real() * rsH_[j]; im += p[j].imag() * rsH_[j]; }
            rsF_.emplace_back(re, im);
        }
        rsHist_.erase(rsHist_.begin(), rsHist_.begin() + long(avail));
    } else rsF_.insert(rsF_.end(), x, x + n);
    // cubic interpolation to 24 kHz
    const double step = rate / kFs;
    while (rsPos_ + 2.0 < double(rsF_.size())) {
        const int i = int(rsPos_);
        const float mu = float(rsPos_ - i);
        const cf y0 = i > 0 ? rsF_[size_t(i - 1)] : rsF_[0], y1 = rsF_[size_t(i)], y2 = rsF_[size_t(i + 1)], y3 = rsF_[size_t(i + 2)];
        const cf a0 = y3 - y2 - y0 + y1, a1 = y0 - y1 - a0, a2 = y2 - y0, a3 = y1;
        buf_.push_back(((a0 * mu + a1) * mu + a2) * mu + a3);
        ++samplesIn24_;
        rsPos_ += step;
    }
    const int drop = std::max(0, int(rsPos_) - 2);
    rsF_.erase(rsF_.begin(), rsF_.begin() + drop);
    rsPos_ -= drop;
}

void Receiver::trimBuffer(int64_t keepFrom) {
    if (keepFrom <= bufBase_) return;
    const int64_t drop = std::min<int64_t>(keepFrom - bufBase_, int64_t(buf_.size()));
    // ★ erase in big bites only: front-erasing a vector per symbol would be quadratic
    if (drop < 4096 && drop < int64_t(buf_.size()) / 2) return;
    buf_.erase(buf_.begin(), buf_.begin() + long(drop));
    bufBase_ += drop;
}

/** ★★ THE PULSE BLANKER. The first real recording (RRI, 9720 kHz, 2026-10-26) carried a radar-like pulse every 25 ms —
 *  3 ms at eight times the signal's amplitude, i.e. NINE times its energy. It drowns every energy-normalised statistic
 *  (the guard correlation read 0.06 instead of ~0.9) and smears into every carrier after the FFT. A sample whose power
 *  stands 13 dB over the running mean of the UNBLANKED signal is zeroed, with a margin either side; OFDM loses those
 *  samples' energy evenly across all carriers, which the code shrugs off — far better than letting the pulse in.
 *  ★★ 13 dB, NOT LESS: an OFDM signal's power is exponentially distributed, so a 9 dB threshold (e^-8 per sample) fired
 *     in one symbol in five on a CLEAN synthetic signal and capped its MER at ~20 dB. 13 dB is e^-20 — never by chance —
 *     and the radar sat 18 dB up.
 *  ★ A level that RISES and stays (a transmitter switching on, a gain change) is not a pulse: after 100 ms of blanking
 *    the mean is taken from the new level.
 *  ★★★ …AND A SIGNAL COMING BACK OUT OF A FADE IS NOT A PULSE EITHER (2026-10-29). The 100 ms mean follows the signal
 *    DOWN into a fade, and on the way back up the signal stood "13 dB over the mean" — and was zeroed, at the moment it
 *    was returning. Annex B's fading channels lost most of their frames to it (a single Rayleigh path at 1 Hz Doppler,
 *    30 dB SNR: 2.3 % blanked, 57 of 140 frames; unblanked, 140 of 140). The reference is now the LARGER of the 100 ms
 *    mean and a 1 s mean: a fade drags the first down, not the second, so the recovery is judged against the signal's
 *    usual level; a fading peak is judged against the 100 ms mean, which has followed it up. A pulse (18 dB on the RRI
 *    recording) stands over both. The 8-sample test sits at 6× (7.8 dB): a Rayleigh channel spends e^-6 = 0.25 % of the
 *    time that far over its own mean. */
void Receiver::blankNew() {
    for (int64_t i = std::max(blankFrom_, bufBase_); i < bufEnd(); ++i) {
        cf& v = buf_[size_t(i - bufBase_)];
        const float p = std::norm(v);
        // ★ a recent-fraction pair of 5 s buckets (the Signal pane's row): what the blanker is doing NOW, not since tuning in
        if (++blankRecentN_[blankBucket_] >= 5 * kFs) { blankBucket_ ^= 1; blankRecentN_[blankBucket_] = 0; blankRecent_[blankBucket_] = 0; }
        /* ★ WARM-UP: the running mean starts as the mean of the first 10 ms, not as the first sample's power. One sample of
         *  an OFDM signal can be near zero, and then everything after it stood "13 dB over the mean" — the first 100 ms
         *  were blanked until the level-rise rule gave up (seen as 10 % blanked in the first second of every lock). */
        if (blankWarm_ < kFs / 100) {
            pAvg_ = blankWarm_ == 0 ? p + 1e-12f : pAvg_ + (p - pAvg_) / float(blankWarm_ + 1);
            pSlow_ = pAvg_;
            ++blankWarm_;
            blankWin_[blankWi_ & 7] = p; ++blankWi_;
            continue;
        }
        // ★ the decision is on an 8-sample average: a pulse is a RUN of high samples, an OFDM peak is one or two
        //   (8 samples ≈ 16 degrees of freedom: 6 dB over the mean by chance is ~1e-6, so the threshold can sit low
        //   enough to catch a pulse's skirts, which a single-sample test at 13 dB let through)
        blankWin_[blankWi_ & 7] = p; ++blankWi_;
        float s8 = 0; for (float q : blankWin_) s8 += q;
        const float ref = std::max(pAvg_, pSlow_);
        if (s8 > 6.0f * 8.0f * ref || p > 20.0f * ref) {
            ++blankRun_;
            if (blankRun_ > kFs / 10) { pAvg_ = pSlow_ = s8 / 8.0f; blankRun_ = 0; continue; }
            for (int64_t j = std::max(bufBase_, i - 8); j < i; ++j) buf_[size_t(j - bufBase_)] = cf(0, 0);
            blankHold_ = 8;
        }
        if (blankHold_ > 0) { --blankHold_; v = cf(0, 0); ++blanked_; ++blankRecent_[blankBucket_]; continue; }
        blankRun_ = 0;
        pAvg_ += (p - pAvg_) * (1.0f / 2400.0f);
        pSlow_ += (p - pSlow_) * (1.0f / 24000.0f);
    }
    blankFrom_ = bufEnd();
}

void Receiver::feed(const cf* iq, int n, double rateHz) {
    if (n <= 0 || !(rateHz >= 11000)) return;
    resampleIn(iq, n, rateHz);
    if (!getenv("VIBE_DRM_NOBLANK")) blankNew();
    for (int guard = 0; guard < 1000; ++guard) {
        const State before = state_;
        const size_t sz = buf_.size(); const int64_t base = bufBase_;
        if (state_ == State::Search) runSearch();
        else if (state_ == State::Freq) runFreq();
        else runTrack();
        if (state_ == before && buf_.size() == sz && bufBase_ == base) break;   // no progress: wait for samples
    }
}

// ── 1. Mode and timing ──────────────────────────────────────────────────────────────────────────────────────────────

void Receiver::runSearch() {
    if (int64_t(buf_.size()) < kAcqLen) return;
    struct Best { double score = 0, median = 0; int r = 0; cf c; } best[4];
    int bestM = -1; double bestScore = 0;
    std::vector<double> p(static_cast<size_t>(kAcqLen)), e(static_cast<size_t>(kAcqLen));
    std::vector<cf> pc(static_cast<size_t>(kAcqLen));
    for (int m = 0; m < 4; ++m) {
        const ModeInfo& mi = modeInfo(Mode(m));
        const int tu = mi.tu, tg = mi.tg, ts = tu + tg;
        const int n = kAcqLen - tu;
        for (int i = 0; i < n; ++i) {
            const cf a = buf_[size_t(i)], b = buf_[size_t(i + tu)];
            pc[size_t(i)] = a * std::conj(b);
            e[size_t(i)] = 0.5 * (double(std::norm(a)) + double(std::norm(b)));
        }
        std::vector<cf> cf_(static_cast<size_t>(ts), cf(0, 0));
        std::vector<double> ef(static_cast<size_t>(ts), 0.0);
        // moving sum over Tg, folded modulo Ts
        cf accC(0, 0); double accE = 0;
        for (int i = 0; i < tg; ++i) { accC += pc[size_t(i)]; accE += e[size_t(i)]; }
        for (int i = 0; i + tg < n; ++i) {
            cf_[size_t(i % ts)] += accC; ef[size_t(i % ts)] += accE;
            accC += pc[size_t(i + tg)] - pc[size_t(i)]; accE += e[size_t(i + tg)] - e[size_t(i)];
        }
        std::vector<double> metric(static_cast<size_t>(ts));
        for (int r = 0; r < ts; ++r) metric[size_t(r)] = ef[size_t(r)] > 0 ? std::abs(cf_[size_t(r)]) / ef[size_t(r)] : 0;
        int rb = int(std::max_element(metric.begin(), metric.end()) - metric.begin());
        // ★ the reference is the fold's 10th percentile, not its median: mode D's guard is 11/14 of a symbol, so its
        //   correlation is a wide triangle and its median sits half-way up the peak
        std::vector<double> sorted = metric;
        std::nth_element(sorted.begin(), sorted.begin() + ts / 10, sorted.end());
        best[m].score = metric[size_t(rb)]; best[m].median = sorted[size_t(ts / 10)]; best[m].r = rb; best[m].c = cf_[size_t(rb)];
        // ★ normalised by the median so a mode is judged by how far its peak STANDS OUT, not how high it is
        const double s = best[m].score - best[m].median;
        if (s > bestScore) { bestScore = s; bestM = m; }
    }
    if (getenv("VIBE_DRM_DEBUG")) for (int m = 0; m < 4; ++m) fprintf(stderr, "[drm] search %c peak %.3f median %.3f r %d\n", 'A' + m, best[m].score, best[m].median, best[m].r);
    const bool ok = bestM >= 0 && best[bestM].score > 0.15 && bestScore > 0.10 && best[bestM].score > 3.0 * best[bestM].median;
    if (!ok) {
        // keep the last half: a signal that began mid-buffer gets a clean look next time
        buf_.erase(buf_.begin(), buf_.begin() + kAcqLen / 2);
        bufBase_ += kAcqLen / 2;
        return;
    }
    mode_ = Mode(bestM);
    const ModeInfo& mi = modeInfo(mode_);
    tu_ = mi.tu; tg_ = mi.tg; ts_ = tu_ + tg_;
    int kmin, kmax; carrierRange(mode_, 3, kmin, kmax); kw_ = kmax;
    fft_.reset(new vibedsp::ComplexFFT(tu_));
    tin_.resize(size_t(tu_)); tout_.resize(size_t(tu_));
    // fractional frequency from the correlation's phase: x[n]·x*[n+Tu] = e^{−j2π f Tu/fs}
    freq_ = -double(std::arg(best[bestM].c)) * kFs / (2.0 * M_PI * tu_);
    const int margin = windowMargin();
    int64_t w = bufBase_ + best[bestM].r + tg_ - margin;
    while (w - ts_ >= bufBase_) w -= ts_;
    winPos_ = w;
    ncoPhase_ = 0; ncoIdx_ = winPos_;
    st_.timeSync = true; st_.mode = mode_; markLock(0);
    state_ = State::Freq;
}

void Receiver::fftSymbol(int64_t pos, std::vector<cf>& car) {
    // NCO: continuous phase across symbols (and across a frequency change, applied from this window on)
    const double ph0 = ncoPhase_ - 2.0 * M_PI * freq_ * double(pos - ncoIdx_) / kFs;
    ncoPhase_ = std::fmod(ph0, 2.0 * M_PI); ncoIdx_ = pos;
    const double dph = -2.0 * M_PI * freq_ / kFs;
    cf rot = std::polar(1.0f, float(ncoPhase_));
    const cf step = std::polar(1.0f, float(dph));
    const cf* src = buf_.data() + (pos - bufBase_);
    for (int i = 0; i < tu_; ++i) {
        tin_[size_t(i)] = src[i] * rot;
        rot *= step;
        if ((i & 63) == 63) rot /= std::abs(rot);
    }
    fft_->forward(tin_.data(), tout_.data());
    car.resize(size_t(2 * kw_ + 1));
    const float sc = 1.0f / std::sqrt(float(tu_));
    for (int k = -kw_; k <= kw_; ++k) car[size_t(k + kw_)] = tout_[size_t((k + tu_) % tu_)] * sc;
}

// ── 2. Integer frequency ────────────────────────────────────────────────────────────────────────────────────────────

void Receiver::runFreq() {
    const ModeInfo& mi = modeInfo(mode_);
    const int J = mi.ns;   // one frame
    if (winPos_ + int64_t(J) * ts_ + tu_ > bufEnd()) return;
    // full-band FFTs (the search needs bins beyond ±kw)
    std::vector<std::vector<cf>> X(static_cast<size_t>(J));
    double ph = 0; int64_t idx = winPos_;
    for (int j = 0; j < J; ++j) {
        const int64_t pos = winPos_ + int64_t(j) * ts_;
        ph -= 2.0 * M_PI * freq_ * double(pos - idx) / kFs; idx = pos;
        cf rot = std::polar(1.0f, float(ph));
        const cf step = std::polar(1.0f, float(-2.0 * M_PI * freq_ / kFs));
        const cf* src = buf_.data() + (pos - bufBase_);
        for (int i = 0; i < tu_; ++i) { tin_[size_t(i)] = src[i] * rot; rot *= step; }
        fft_->forward(tin_.data(), tout_.data());
        X[size_t(j)] = tout_;
    }
    const double spacing = double(kFs) / tu_;
    const int dmax = int(kSearchSpanHz / spacing);
    std::vector<double> met(size_t(2 * dmax + 1), 0.0);
    for (int d = -dmax; d <= dmax; ++d) {
        // ★ coherence PER TONE, then averaged: one tone landing on another (d = 32 maps 16 onto 48) must score ⅓, not 1
        double sum = 0;
        for (int q = 0; q < 3; ++q) {
            const int bin = ((mi.fp[q] + d) % tu_ + tu_) % tu_;
            const bool alt = mode_ == Mode::D && (mi.fp[q] == 7 || mi.fp[q] == 21);
            cf a(0, 0); double en = 0;
            // ★ an offset of d WHOLE carriers still turns each symbol by 2π·d·Tg/Tu (the guard): undo it per hypothesis
            const cf turn = std::polar(1.0f, float(-2.0 * M_PI * d * tg_ / tu_));
            cf r(1, 0);
            for (int j = 0; j < J; ++j) { const cf v = X[size_t(j)][size_t(bin)] * r; a += (alt && (j & 1)) ? -v : v; en += std::norm(v); r *= turn; }
            sum += en > 0 ? std::norm(a) / (en * J) : 0;
        }
        met[size_t(d + dmax)] = sum / 3.0;
    }
    const int bi = int(std::max_element(met.begin(), met.end()) - met.begin());
    double second = 0;
    for (int i = 0; i < int(met.size()); ++i) if (std::abs(i - bi) > 1) second = std::max(second, met[size_t(i)]);
    if (getenv("VIBE_DRM_DEBUG")) fprintf(stderr, "[drm] freq: best d=%d coh %.3f second %.3f (frac %.2f Hz)\n", bi - dmax, met[size_t(bi)], second, freq_);
    if (met[size_t(bi)] < 0.35 || met[size_t(bi)] < 2.0 * second) {
        loseSync("freq");
        // ★ move on: the same buffer would fail the same way
        const int64_t drop = std::min<int64_t>(kAcqLen / 2, int64_t(buf_.size()));
        buf_.erase(buf_.begin(), buf_.begin() + long(drop)); bufBase_ += drop;
        return;
    }
    freq_ += (bi - dmax) * spacing;
    ncoPhase_ = 0; ncoIdx_ = winPos_;
    st_.freqSync = true; markLock(1);
    state_ = State::Track;
    trackStart_ = winPos_;
    tScore_.assign(size_t(mi.ns), 0.0); symCount_ = 0; frameSynced_ = false;
    havePrev_ = false;
}

// ── 3–6. Tracking ─────────────────────────────────────────────────────────────────────────────────────────────────

void Receiver::runTrack() {
    const ModeInfo& mi = modeInfo(mode_);
    std::vector<cf> car;
    while (winPos_ + tu_ <= bufEnd() && state_ == State::Track) {
        fftSymbol(winPos_, car);
        const int stepUsed = pendingStep_;
        // frequency tracking from the reference tones' symbol-to-symbol phase step
        if (havePrev_) {
            cf acc(0, 0);
            for (int q = 0; q < 3; ++q) {
                const int k = mi.fp[q];
                const bool alt = mode_ == Mode::D && (k == 7 || k == 21);
                if (alt && !frameSynced_) continue;
                cf prev = prevSym_[size_t(k + kw_)] * std::polar(1.0f, float(2.0 * M_PI * k * prevStep_ / tu_));
                cf v = car[size_t(k + kw_)] * std::conj(prev);
                if (alt) v = -v;
                acc += v;
            }
            const double ferr = double(std::arg(acc)) * kFs / (2.0 * M_PI * ts_);
            static const bool noFt = getenv("VIBE_DRM_NOFT") != nullptr;
            if (!noFt) freq_ += 0.05 * ferr;
            st_.freqOffsetHz = freq_;
        }
        prevSym_ = car; havePrev_ = true; prevStep_ = 0;

        if (!frameSynced_) {
            // ★★ FRAME SYNC FROM THE GAIN REFERENCES, DIFFERENTIALLY IN TIME. Their phases depend on the FULL symbol
            //    number (Z256[n][m], m = s / y — §8.4.4.3), so the step between a carrier's pilot now and its pilot y
            //    symbols ago names s. Comparing a carrier with ITSELF 80 ms earlier cancels the channel however
            //    frequency-selective it is; the time references (compared carrier to neighbour) could not survive annex
            //    B's channel 5 — two paths 4 ms apart turn adjacent carriers' phases by a different amount each.
            // ★ …but the step only names s up to the gain pattern's row differences, which in modes C and D repeat (W
            //   falls by a constant per row), so the time references' carrier-to-neighbour score is ADDED: it pins the
            //   exact symbol in a mild channel, the differential keeps the alignment right in a harsh one.
            {
                const auto& tr = timeRefs(mode_);
                const int margin = windowMargin();
                cf s(0, 0); double nrm = 0;
                for (size_t i = 0; i + 1 < tr.size(); ++i) {
                    const int ka = tr[i].k, kb = tr[i + 1].k;
                    const cf xa = car[size_t(ka + kw_)] * std::polar(1.0f, float(2.0 * M_PI * ka * margin / tu_));
                    const cf xb = car[size_t(kb + kw_)] * std::polar(1.0f, float(2.0 * M_PI * kb * margin / tu_));
                    const cf ref = phasor(tr[i + 1].ph, 1.0f) * std::conj(phasor(tr[i].ph, 1.0f));
                    s += xb * std::conj(xa) * std::conj(ref);
                    nrm += std::abs(xa) * std::abs(xb);
                }
                tScore_[size_t(symCount_ % mi.ns)] += nrm > 0 ? 2.0 * std::abs(s) / nrm : 0;
            }
            if (fsMap_.mode != mode_) fsMap_.build(mode_, 3);
            fsHist_.push_back(car);
            if (int(fsHist_.size()) > mi.y + 1) fsHist_.pop_front();
            if (int(fsHist_.size()) == mi.y + 1) {
                const std::vector<cf>& old = fsHist_.front();
                for (int h = 0; h < mi.ns; ++h) {
                    const int sp = ((h - mi.y) % mi.ns + mi.ns) % mi.ns;
                    cf acc(0, 0); double nrm = 0;
                    for (int k = fsMap_.kmin; k <= fsMap_.kmax; ++k) {
                        if (!fsMap_.onGainGrid(h, k)) continue;
                        const cf p1 = fsMap_.pilot[size_t(fsMap_.idx(1, h, k))], p0 = fsMap_.pilot[size_t(fsMap_.idx(1, sp, k))];
                        if (std::norm(p1) == 0 || std::norm(p0) == 0) continue;
                        const cf v = car[size_t(k + kw_)] * std::conj(old[size_t(k + kw_)]);
                        acc += v * std::conj(p1 * std::conj(p0));
                        nrm += std::abs(v) * std::abs(p1) * std::abs(p0);
                    }
                    const int o = ((symCount_ - h) % mi.ns + mi.ns) % mi.ns;
                    tScore_[size_t(o)] += nrm > 0 ? std::abs(acc) / nrm : 0;
                }
            }
            ++symCount_;
            if (symCount_ >= 2 * mi.ns && symCount_ % mi.ns == 0) {
                const int b = int(std::max_element(tScore_.begin(), tScore_.end()) - tScore_.begin());
                double others = 0, second = 0;
                for (int i = 0; i < mi.ns; ++i) if (i != b) { others += tScore_[size_t(i)]; second = std::max(second, tScore_[size_t(i)]); }
                others /= (mi.ns - 1);
                if (getenv("VIBE_DRM_DEBUG")) { fprintf(stderr, "[drm] frame scores:"); for (double v : tScore_) fprintf(stderr, " %.1f", v); fprintf(stderr, "\n"); }
                // ★ a wrong choice costs one resync (the FAC's CRC fails and says so); a missed one costs the same
                if (tScore_[size_t(b)] > 1.5 * others && tScore_[size_t(b)] > 1.12 * second) {
                    frameSynced_ = true;
                    // the NEXT symbol (index symCount_) has local position symCount_ % ns; s = that − b
                    sIdx_ = ((symCount_ - b) % mi.ns + mi.ns) % mi.ns;
                    gIdx_ = int64_t(mi.ns) * 1000 + sIdx_;
                    st_.frameSync = true; markLock(2);
                    estSo_ = 3; emap_.build(mode_, estSo_); buildWiener(); delayPlan();
                    ring_.clear(); frS_ = 0;
                    frY_.assign(size_t(emap_.ns * emap_.nk), cf(0, 0)); frW_.assign(frY_.size(), 0.0f);
                    for (auto& f : sf_) f.have = false;
                } else if (symCount_ >= 6 * mi.ns) { loseSync("frame"); return; }
            }
            winPos_ += ts_; trimBuffer(winPos_ - ts_);
            continue;
        }
        pushEstimator(car);
        if (state_ != State::Track) return;
        ++gIdx_; sIdx_ = (sIdx_ + 1) % mi.ns;
        // timing step decided by the estimator — applied to the next window
        winPos_ += ts_ + pendingStep_;
        if (pendingStep_) { prevStep_ = pendingStep_; stepSum_ += pendingStep_; pendingStep_ = 0; }
        (void)stepUsed;
        const int64_t elapsed = winPos_ - trackStart_;
        if (elapsed > kFs * 10) st_.sroPpm = double(stepSum_) / double(elapsed) * 1e6;
        trimBuffer(winPos_ - ts_);
    }
}

/** ★★ THE FREQUENCY INTERPOLATOR IS A WIENER FILTER, NOT A STRAIGHT LINE. Annex B's channel 5 (two equal paths 4 ms
 *  apart) ripples across the band with a period of 5⅓ carriers in mode B, where the scattered pilots, once interpolated
 *  in time, sit every 2: a straight line between them is wrong by most of the ripple (FAC MER −5 dB, nothing decoded).
 *  The standard's design limit is the guard interval, so the channel's power-delay profile is modelled as flat over
 *  Tg about its mean (which the slope removal has put at zero): correlation sinc(Δk·Tg/Tu) between carriers Δk apart.
 *  Each carrier's estimate is the MMSE combination of its kWienerK nearest pilot columns (noise assumed 20 dB down),
 *  solved once per plan — at the band edges the taps are simply one-sided.
 *  ★ That flat model is only the START (2026-10-29): once the averaged delay profile has ~20 symbols in it, updateWiener
 *    replaces it every frame with the channel's own measured profile and noise floor. */
void Receiver::buildWiener() {
    const ModeInfo& mi = modeInfo(mode_);
    wienerCols_.clear();
    for (int j = 0; j < emap_.nk; ++j) {
        const int k = emap_.kmin + j;
        if (carrierUnused(mode_, k)) continue;
        if (((k - mi.k0) % mi.x + mi.x) % mi.x != 0) continue;
        wienerCols_.push_back(j);
    }
    // each carrier's kWienerK nearest columns, once per plan
    const int K = kWienerK, nc = int(wienerCols_.size());
    wienerGeom_.assign(size_t(emap_.nk), -1); wienerPat_.clear(); wienerDmax_ = 0;
    for (int j = 0; j < emap_.nk; ++j) {
        std::vector<int> idx(static_cast<size_t>(nc));
        for (int i = 0; i < nc; ++i) idx[size_t(i)] = i;
        const int kk = std::min(K, nc);
        std::partial_sort(idx.begin(), idx.begin() + kk, idx.end(),
                          [&](int a, int b) { return std::abs(wienerCols_[size_t(a)] - j) < std::abs(wienerCols_[size_t(b)] - j); });
        std::vector<int> o(static_cast<size_t>(kk));
        for (int a = 0; a < kk; ++a) o[size_t(a)] = wienerCols_[size_t(idx[size_t(a)])] - j;
        for (int a = 0; a < kk; ++a) for (int b = 0; b < kk; ++b) wienerDmax_ = std::max({ wienerDmax_, std::abs(o[size_t(a)]), std::abs(o[size_t(a)] - o[size_t(b)]) });
        auto it = std::find(wienerPat_.begin(), wienerPat_.end(), o);
        if (it == wienerPat_.end()) { wienerPat_.push_back(o); it = wienerPat_.end() - 1; }
        wienerGeom_[size_t(j)] = kk ? int(it - wienerPat_.begin()) : -1;
    }
    // the design model until the channel's own profile has been measured: flat over Tg about the centre, 20 dB SNR
    std::vector<cf> rho(size_t(2 * emap_.nk + 1));
    const double span = double(tg_) / double(tu_);
    for (int d = -emap_.nk; d <= emap_.nk; ++d) {
        const double x = M_PI * d * span;
        rho[size_t(d + emap_.nk)] = cf(float(std::abs(x) < 1e-9 ? 1.0 : std::sin(x) / x), 0.0f);
    }
    solveWiener(rho, 0.01);
}

/** The MMSE interpolator for correlation `rho` (indexed Δk + nk, unit at 0) and pilot noise-to-signal ratio `nr`: each
 *  carrier from its nearest columns. Orthogonality, E[(H − Σ c_a y_a) y_b*] = 0, gives Σ_a c_a R_ab = r_b with
 *  R_ab = ρ(c_a − c_b) + nr·δ_ab and r_b = ρ(k − c_b); solved by Gaussian elimination per carrier. */
void Receiver::solveWiener(const std::vector<cf>& rho, double nr) {
    const int K = kWienerK, nk = emap_.nk;
    wienerTaps_.assign(size_t(nk) * K, WienerTap{});
    auto R = [&](int d) { return std::complex<double>(rho[size_t(d + nk)]); };
    std::vector<std::complex<double>> A(static_cast<size_t>(K * K)), rhs(static_cast<size_t>(K)), w(static_cast<size_t>(K));
    // ★ the taps depend only on the columns' OFFSETS from the carrier (ρ is a function of Δk), and inside the band those
    //   repeat every x carriers: one solve per distinct pattern (~40 in mode B SO 3), not one per carrier (207)
    std::vector<std::vector<cf>> pw(wienerPat_.size());
    for (size_t pi = 0; pi < wienerPat_.size(); ++pi) {
        const std::vector<int>& o = wienerPat_[pi];
        const int kk = int(o.size());
        // row b is the equation for column b: Σ_a R_ab c_a = r_b, R_ab = ρ(o_a − o_b) + nr·δ_ab, r_b = ρ(−o_b)
        for (int r = 0; r < kk; ++r) {
            rhs[size_t(r)] = R(-o[size_t(r)]);
            for (int c = 0; c < kk; ++c) A[size_t(r * kk + c)] = R(o[size_t(c)] - o[size_t(r)]) + (c == r ? nr : 0.0);
        }
        for (int c = 0; c < kk; ++c) {
            int piv = c; for (int r = c + 1; r < kk; ++r) if (std::abs(A[size_t(r * kk + c)]) > std::abs(A[size_t(piv * kk + c)])) piv = r;
            if (piv != c) { for (int q = 0; q < kk; ++q) std::swap(A[size_t(c * kk + q)], A[size_t(piv * kk + q)]); std::swap(rhs[size_t(c)], rhs[size_t(piv)]); }
            for (int r = c + 1; r < kk; ++r) {
                const std::complex<double> f = A[size_t(r * kk + c)] / A[size_t(c * kk + c)];
                for (int q = c; q < kk; ++q) A[size_t(r * kk + q)] -= f * A[size_t(c * kk + q)];
                rhs[size_t(r)] -= f * rhs[size_t(c)];
            }
        }
        for (int c = kk - 1; c >= 0; --c) {
            std::complex<double> v = rhs[size_t(c)];
            for (int q = c + 1; q < kk; ++q) v -= A[size_t(c * kk + q)] * w[size_t(q)];
            w[size_t(c)] = v / A[size_t(c * kk + c)];
        }
        pw[pi].resize(size_t(kk));
        for (int c = 0; c < kk; ++c) pw[pi][size_t(c)] = cf(float(w[size_t(c)].real()), float(w[size_t(c)].imag()));
    }
    for (int j = 0; j < nk; ++j) {
        const int pi = wienerGeom_[size_t(j)];
        if (pi < 0) continue;
        const std::vector<int>& o = wienerPat_[size_t(pi)];
        for (int a = 0; a < K; ++a) {
            WienerTap& t = wienerTaps_[size_t(j * K + a)];
            if (a < int(o.size())) { t.col = j + o[size_t(a)]; t.w = pw[size_t(pi)][size_t(a)]; }
            else { t.col = j + o[0]; t.w = cf(0, 0); }
        }
    }
}

/** ★★ THE INTERPOLATOR FOLLOWS THE CHANNEL'S MEASURED DELAY PROFILE (2026-10-29, the French broadcast on 5970 kHz).
 *  The design model — flat over Tg — throws away whatever the channel holds outside it, and night-time HF can hold a
 *  lot: diffuse scatter spread over most of the profile's span. The averaged profile pdp_ the timing loop already keeps
 *  IS the channel's power-delay profile plus a flat noise floor, so the textbook MMSE interpolator can be built from it:
 *  correlation ρ(Δk) = Σ_t P(t)·e^{−j2πΔk(t−τc)/Tu} (τc: the derotation centre), noise from the floor.
 *  Every other frame (800 ms: the profile itself is a ~1 s average); the whole of it costs ~5 % of the receiver's CPU. */
void Receiver::updateWiener() {
    static const bool flatOnly = getenv("VIBE_DRM_WFLAT") != nullptr;   // ★ bench knob: the design model only, A/B
    if (flatOnly || (++wienerTick_ & 1)) return;
    const int P = int(pdp_.size());
    if (P == 0 || pdpN_ < 20 || wienerCols_.empty() || wienerPat_.empty() || twid_.size() != size_t(tu_)) return;
    // the noise floor: a low percentile (paths occupy part of the span, noise all of it)
    std::vector<float> tmp = pdp_;
    std::nth_element(tmp.begin(), tmp.begin() + P / 5, tmp.end());
    const double floor = std::max(1e-30, double(tmp[size_t(P / 5)]));
    std::vector<double> Pm(static_cast<size_t>(P));
    double sig = 0;
    for (int t = 0; t < P; ++t) { const double v = double(pdp_[size_t(t)]) - floor; Pm[size_t(t)] = v > floor ? v : 0.0; sig += Pm[size_t(t)]; }
    if (!(sig > 0)) return;
    const int nk = emap_.nk, tc = int(std::lround(tauAvg_));
    std::vector<cf> rho(size_t(2 * nk + 1), cf(0, 0));
    for (int t = 0; t < P; ++t) {
        if (Pm[size_t(t)] <= 0) continue;
        int dl = t - tc; while (dl > P / 2) dl -= P; while (dl <= -P / 2) dl += P;
        const float wgt = float(Pm[size_t(t)] / sig);
        for (int d = -wienerDmax_; d <= wienerDmax_; ++d)   // e^{−j2π d·dl/Tu} = conj(twid_[(d·dl) mod Tu]); only the lags the taps use
            rho[size_t(d + nk)] += wgt * std::conj(twid_[size_t(((int64_t(d) * dl) % tu_ + tu_) % tu_)]);
    }
    // pilot noise-to-signal: per tap the floor is Ncols·σ², and the profile's signal sums to P·Ncols·S
    const double nr = std::min(1.0, std::max(1e-3, floor * P / sig));
    solveWiener(rho, nr);
}

void Receiver::applyStepToRing(int delta) {
    // a window moved later by δ multiplies every carrier by e^{+j2πkδ/Tu}: bring the stored pilots along
    for (Sym& sy : ring_)
        for (int j = 0; j < emap_.nk; ++j) {
            const int k = emap_.kmin + j;
            const cf r = std::polar(1.0f, float(2.0 * M_PI * k * delta / tu_));
            sy.Hp[size_t(j)] *= r; sy.X[size_t(j)] *= r;
        }
    /* ★★★ …AND THE AVERAGED DELAY PROFILES (2026-10-26, found by the Signal pane's delay-spread test). A window moved
     *  later by δ sees every path δ samples EARLIER. The ring was brought along, but pdp_ — a ~1.5 s average, which
     *  steers this very loop — was not: it went on reporting the old position, so the loop stepped again on the next
     *  symbol, and the next, ~30 times (one sample per symbol, 30 samples overshoot on a static two-path channel), then
     *  had to walk back. The wandering window smeared the profile it steered from, which made it wander again. Shifting
     *  both profiles with the window keeps them true to it: one step per sample of real drift. */
    auto shift = [delta](std::vector<float>& v) {
        if (v.empty()) return;
        if (delta > 0) std::rotate(v.begin(), v.begin() + 1, v.end());
        else if (delta < 0) std::rotate(v.begin(), v.end() - 1, v.end());
    };
    shift(pdp_); shift(dsPdp_);
}

void Receiver::pushEstimator(std::vector<cf>& car) {
    const ModeInfo& mi = modeInfo(mode_);
    Sym sy; sy.g = gIdx_; sy.s = sIdx_;
    sy.X.resize(size_t(emap_.nk)); sy.Hp.assign(size_t(emap_.nk), cf(0, 0)); sy.hasP.assign(size_t(emap_.nk), 0);
    for (int j = 0; j < emap_.nk; ++j) {
        const int k = emap_.kmin + j;
        sy.X[size_t(j)] = car[size_t(k + kw_)];
        if (emap_.onGainGrid(sy.s, k)) {
            const cf p = emap_.pilot[size_t(emap_.idx(1, sy.s, k))];
            if (std::norm(p) > 0) { sy.Hp[size_t(j)] = sy.X[size_t(j)] / p; sy.hasP[size_t(j)] = 1; }
        }
    }
    if (!ring_.empty() && ring_.back().g + 1 != sy.g) ring_.clear();
    ring_.push_back(std::move(sy));
    while (int(ring_.size()) > 3 * mi.y + 2) ring_.pop_front();
    static const bool noStats = getenv("VIBE_DRM_NOSTATS") != nullptr;   // ★ bench knob: the Signal pane's cost, A/B
    if (!noStats) dopplerAccumulate(ring_.back());
    // the symbol y−1 back now has its next pilots
    const int64_t gOut = gIdx_ - (2 * mi.y - 1);   // ★ two pilot periods of look-ahead: the cubic needs the pilot after next
    if (ring_.front().g > gOut) return;
    const Sym& out = ring_[size_t(gOut - ring_.front().g)];
    std::vector<cf> Y; std::vector<float> W;
    equalise(out, Y, W);
    // the frame buffer: symbol out.s
    if (out.s == 0 && frS_ != 0) frS_ = 0;
    if (out.s != frS_) { frS_ = out.s; }
    std::copy(Y.begin(), Y.end(), frY_.begin() + long(out.s) * emap_.nk);
    std::copy(W.begin(), W.end(), frW_.begin() + long(out.s) * emap_.nk);
    frS_ = out.s + 1;
    if (out.s == mi.ns - 1) frameDone();
}

void Receiver::equalise(const Sym& sy, std::vector<cf>& Y, std::vector<float>& W) {
    const ModeInfo& mi = modeInfo(mode_);
    const int nk = emap_.nk;
    std::vector<cf> H(size_t(nk), cf(0, 0));
    std::vector<int> cols;
    const int64_t g0 = ring_.front().g;
    for (int j = 0; j < nk; ++j) {
        const int k = emap_.kmin + j;
        if (carrierUnused(mode_, k)) continue;
        const int r = ((k - mi.k0) % mi.x + mi.x) % mi.x;
        if (r != 0) continue;
        const int ph = (((k - mi.k0) / mi.x) % mi.y + mi.y) % mi.y;
        const int d = ((sy.s - ph) % mi.y + mi.y) % mi.y;
        const int64_t gp = sy.g - d, gn = gp + mi.y;
        const bool hp = gp >= g0 && ring_[size_t(gp - g0)].hasP[size_t(j)];
        const bool hn = gn - g0 < int64_t(ring_.size()) && gn >= g0 && ring_[size_t(gn - g0)].hasP[size_t(j)];
        // ★★ CUBIC IN TIME (Lagrange through the pilots at gp−y, gp, gn, gn+y) where all four are in: a fading HF path
        //    turns by a radian between pilots 80 ms apart at 2 Hz Doppler spread (annex B channels 4/5), which a straight
        //    line between two pilots cuts across. Linear at the edges of what is buffered.
        const int64_t gpp = gp - mi.y, gnn = gn + mi.y;
        auto has = [&](int64_t g) { return g >= g0 && g - g0 < int64_t(ring_.size()) && ring_[size_t(g - g0)].hasP[size_t(j)]; };
        auto Hof = [&](int64_t g) { return ring_[size_t(g - g0)].Hp[size_t(j)]; };
        if (d == 0 && hp) H[size_t(j)] = Hof(gp);
        else if (hp && hn && has(gpp) && has(gnn)) {
            const float t = float(d) / mi.y;
            const float wm = -t * (t - 1) * (t - 2) / 6, w0 = (t + 1) * (t - 1) * (t - 2) / 2, w1 = -(t + 1) * t * (t - 2) / 2, w2 = (t + 1) * t * (t - 1) / 6;
            H[size_t(j)] = Hof(gpp) * wm + Hof(gp) * w0 + Hof(gn) * w1 + Hof(gnn) * w2;
        } else if (hp && hn) {
            const float a = float(d) / mi.y;
            H[size_t(j)] = Hof(gp) * (1.0f - a) + Hof(gn) * a;
        } else if (hp) H[size_t(j)] = Hof(gp);
        else if (hn) H[size_t(j)] = Hof(gn);
        else continue;
        cols.push_back(j);
    }
    if (cols.size() < 2) { Y.assign(size_t(nk), cf(0, 0)); W.assign(size_t(nk), 0.0f); return; }
    // ★ the Signal pane's delay spread: every y-th symbol, once every column has its pilot (the full plan)
    static const bool noStats = getenv("VIBE_DRM_NOSTATS") != nullptr;
    if (!noStats && sy.g % mi.y == 0 && int(cols.size()) == dsCols_) delayAccumulate(H, cols);
    // ★★ THE CHANNEL'S DELAY PROFILE from the pilot columns (a DFT over them: period Tu/x samples, ≥ 2·Tg in every mode),
    //    averaged over ~10 symbols. Timing holds the centre of the strongest Tg-long block of it at the middle of the guard;
    //    the frequency interpolator is centred on the same point. A single "mean phase slope" did both jobs until annex B's
    //    channel 5: two equal paths 4 ms apart, fading, made the slope wrap between them and the timing chase it.
    const int P = tu_ / mi.x;
    if (int(pdp_.size()) != P) { pdp_.assign(static_cast<size_t>(P), 0.0f); pdpN_ = 0; }
    {
        if (twid_.size() != size_t(tu_)) { twid_.resize(size_t(tu_)); for (int i = 0; i < tu_; ++i) twid_[size_t(i)] = std::polar(1.0f, float(2.0 * M_PI * i / tu_)); }
        // ★ ~1.5 s memory: the AVERAGE profile. Fading moves power between paths within a second; the paths themselves
        //   do not move, and it is their extent the window must cover
        const float a = pdpN_ < 20 ? 1.0f / float(pdpN_ + 1) : 0.025f;
        for (int t = 0; t < P; ++t) {
            cf acc(0, 0);
            for (int c : cols) {
                const int k = emap_.kmin + c;
                acc += H[size_t(c)] * twid_[size_t(((int64_t(k) * t) % tu_ + tu_) % tu_)];
            }
            pdp_[size_t(t)] = (1.0f - a) * pdp_[size_t(t)] + a * std::norm(acc);
        }
        ++pdpN_;
    }
    // the strongest Tg-long block (a sliding sum: O(P), not O(P·Tg))
    std::vector<double> blockE(static_cast<size_t>(P));
    {
        double e = 0;
        for (int i = 0; i < tg_; ++i) e += pdp_[size_t(i % P)];
        for (int st = 0; st < P; ++st) { blockE[size_t(st)] = e; e += pdp_[size_t((st + tg_) % P)] - pdp_[size_t(st)]; }
    }
    const double bestE = *std::max_element(blockE.begin(), blockE.end());
    // ★ the centre of a block's EXTENT (first to last tap within 15 dB of the strongest), not its power centroid: a power
    //   centroid swings toward whichever path is up, and the window would chase the fading out of the guard.
    //   O(P) for every block at once: for each position, the nearest tap above the threshold at or after it (nxt) and at or
    //   before it (prv), over the profile laid out twice so a block may wrap. (The threshold is the strongest tap's — the
    //   blocks that compete below all hold it.)
    const double pkAll = *std::max_element(pdp_.begin(), pdp_.end());
    std::vector<int> nxt(static_cast<size_t>(2 * P + 1)), prv(static_cast<size_t>(2 * P));
    nxt[size_t(2 * P)] = 1 << 29;
    for (int i = 2 * P - 1; i >= 0; --i) nxt[size_t(i)] = pdp_[size_t(i % P)] > 0.03 * pkAll ? i : nxt[size_t(i + 1)];
    for (int i = 0, last = -(1 << 29); i < 2 * P; ++i) { if (pdp_[size_t(i % P)] > 0.03 * pkAll) last = i; prv[size_t(i)] = last; }
    auto centreOf = [&](int st) {
        const int f = nxt[size_t(st)], l = prv[size_t(st + tg_ - 1)];
        double c = f <= st + tg_ - 1 && l >= st ? 0.5 * (f + l) : st + tg_ / 2.0;
        // wrap into (−P/2, P/2] about the target
        while (c - windowMargin() > P / 2.0) c -= P;
        while (c - windowMargin() <= -P / 2.0) c += P;
        return c;
    };
    /* ★★★ AMONG BLOCKS AS STRONG AS THE STRONGEST, THE ONE THE WINDOW ALREADY HOLDS (2026-10-26). The profile wraps every
     *  Tu/x samples, and where the guard is most of that span (mode D: Tg 176 of 224) two paths can be read EITHER WAY
     *  ROUND — 4 ms apart, or the other 5.3 ms late — and both readings fit in a Tg-long block with the same energy.
     *  "Strongest" was then a coin toss decided by noise, and each toss threw the window 108 samples: the true early
     *  path fell outside the guard (annex B channel 5 in mode D). Within 5 % of the strongest, the reading nearest the
     *  window's own centre wins; a block that is really stronger still takes over. */
    double tauC = 0, bestD = 1e30;
    for (int st = 0; st < P; ++st) {
        if (blockE[size_t(st)] < 0.95 * bestE) continue;
        const double c = centreOf(st);
        const double dd = std::abs(c - windowMargin());
        if (dd < bestD) { bestD = dd; tauC = c; }
    }
    const float theta = float(-2.0 * M_PI * tauC / tu_);
    // derotate, interpolate between columns, rotate back
    std::vector<cf> Hd(static_cast<size_t>(nk), cf(0, 0));
    for (int c : cols) Hd[size_t(c)] = H[size_t(c)] * std::polar(1.0f, -theta * float(emap_.kmin + c));
    if (int(cols.size()) == int(wienerCols_.size()) && std::equal(cols.begin(), cols.end(), wienerCols_.begin())) {
        // ★ the Wiener interpolator (built once per plan): every carrier from its nearest columns
        std::vector<cf> Hi(static_cast<size_t>(nk), cf(0, 0));
        for (int j = 0; j < nk; ++j) {
            const WienerTap* t = &wienerTaps_[size_t(j) * kWienerK];
            cf acc(0, 0);
            for (int q = 0; q < kWienerK; ++q) acc += Hd[size_t(t[q].col)] * t[q].w;
            Hi[size_t(j)] = acc;
        }
        Hd.swap(Hi);
    } else {
        // the first symbols after a (re)plan, before every column has its pilot: linear
        for (size_t i = 0; i + 1 < cols.size(); ++i) {
            const int a = cols[i], b = cols[i + 1];
            for (int j = a + 1; j < b; ++j) { const float t = float(j - a) / float(b - a); Hd[size_t(j)] = Hd[size_t(a)] * (1.0f - t) + Hd[size_t(b)] * t; }
        }
        for (int j = 0; j < cols.front(); ++j) Hd[size_t(j)] = Hd[size_t(cols.front())];
        for (int j = cols.back() + 1; j < nk; ++j) Hd[size_t(j)] = Hd[size_t(cols.back())];
    }
    for (int j = 0; j < nk; ++j) H[size_t(j)] = Hd[size_t(j)] * std::polar(1.0f, theta * float(emap_.kmin + j));
    Y.resize(size_t(nk)); W.resize(size_t(nk));
    equaliseCells(sy.X.data(), H.data(), Y.data(), W.data(), nk);
    if (getenv("VIBE_DRM_DEBUG2")) {
        double pe = 0, pp = 0; int np = 0;
        for (int j = 0; j < nk; ++j) if (sy.hasP[size_t(j)]) {
            const cf pv = emap_.pilot[size_t(emap_.idx(1, sy.s, emap_.kmin + j))];
            pe += std::norm(Y[size_t(j)] - pv); pp += std::norm(pv); ++np;
        }
        static int cnt = 0;
        if (++cnt % 37 == 0) fprintf(stderr, "[drm] g %lld s %d pilotMER %.1f dB tau %.2f theta %.4f step %d\n", (long long)sy.g, sy.s, 10 * std::log10(pp / std::max(pe, 1e-30)), tauAvg_, theta, pendingStep_);
    }
    // timing: the delay profile's centre, held at the window margin (the middle of the guard)
    const double tau = tauC;
    tauAvg_ = tau; ++tauN_;
    const double target = windowMargin();
    static const bool noTt = getenv("VIBE_DRM_NOTT") != nullptr;
    if (tauN_ > 8 && pendingStep_ == 0 && !noTt) {
        if (tauAvg_ > target + 1.0) { pendingStep_ = +1; tauAvg_ -= 1.0; applyStepToRing(+1); }
        else if (tauAvg_ < target - 1.0) { pendingStep_ = -1; tauAvg_ += 1.0; applyStepToRing(-1); }
    }
}

// ── FAC per frame ───────────────────────────────────────────────────────────────────────────────────────────────

void Receiver::frameDone() {
    updateWiener();
    channelStats();   // ★ the Signal pane's delay spread and Doppler, once per frame (400 ms) — a frame whose FAC fails too
    // the FAC cells in map order
    std::vector<cf> fy; std::vector<float> fw;
    for (int s = 0; s < emap_.ns; ++s)
        for (int k = emap_.kmin; k <= emap_.kmax; ++k)
            if (emap_.type[size_t(emap_.idx(1, s, k))] == kFac) {
                const size_t i = size_t(s * emap_.nk + (k - emap_.kmin));
                fy.push_back(frY_[i]); fw.push_back(frW_[i]);
            }
    if (facL_.n2 != 65) facL_.buildFac();
    Fac fac; bool ok = false;
    if (fy.size() == 65) {
        std::vector<uint8_t> u = mlc_.decode(fy.data(), fw.data(), facL_, 1);
        prbs(u.data(), u.size());
        ok = fac.fromBits(u.data());
        // MER on the FAC (4-QAM)
        double ps = 0, pe = 0;
        for (const cf& y : fy) { const cf d = nearestQam(y, 1); ps += std::norm(d); pe += std::norm(y - d); }
        if (getenv("VIBE_DRM_DEBUG2")) {
            int q = 0;
            for (int s2 = 0; s2 < emap_.ns; ++s2) for (int k = emap_.kmin; k <= emap_.kmax; ++k)
                if (emap_.type[size_t(emap_.idx(1, s2, k))] == kFac) { const cf y = fy[size_t(q++)]; const cf d = nearestQam(y, 1);
                    if (std::norm(y - d) > 0.01) fprintf(stderr, "[drm] FAC cell s%d k%d y=(%.3f,%.3f)\n", s2, k, y.real(), y.imag()); }
        }
        const float mer = pe > 0 ? float(10.0 * std::log10(ps / pe)) : 40.0f;
        // ★ only frames whose FAC decoded: the frame after a (re)plan is half empty and would read as a terrible MER
        if (ok) { facMerAcc_ = merInit_ ? 0.8f * facMerAcc_ + 0.2f * mer : mer; merInit_ = true; }
        st_.facMerDb = facMerAcc_; st_.wmerDb = facMerAcc_;
    }
    if (ok && haveFac_ && (fac.so != lastFac_.so || fac.mscMode != lastFac_.mscMode) && facGoodRun_ < 2) {
        // ★ two FACs disagree before lock: believe neither yet
        facGoodRun_ = 0;
    }
    int f;
    if (ok) {
        ++st_.facGood; facFailRun_ = 0; ++facGoodRun_;
        f = fac.frameInSuper();
        lastFac_ = fac; haveFac_ = true;
        st_.facOk = true; st_.fac = fac; markLock(3);
        if (facGoodRun_ >= 2) st_.locked = true;
        st_.so = fac.so; st_.mscLevels = fac.mscLevels(); st_.sdcLevels = fac.sdcLevels(); st_.longInterleave = fac.longInterleave;
    } else {
        ++st_.facBad; ++facFailRun_; facGoodRun_ = 0; st_.facOk = false;
        f = lastFrameIdx_ >= 0 ? (lastFrameIdx_ + 1) % 3 : -1;
        if (facFailRun_ > (st_.locked ? 15 : 6)) { loseSync("fac"); if (onSuperFrame) onSuperFrame(); return; }
    }
    lastFrameIdx_ = f;
    // ★ the FAC names the occupancy: re-plan the estimator on it (and drop what was gathered on the wrong plan)
    if (ok && fac.so != estSo_) {
        if (!occupancySupported(mode_, fac.so)) { st_.mscOk = false; return; }
        estSo_ = fac.so; emap_.build(mode_, estSo_); buildWiener(); delayPlan();
        ring_.clear();
        frY_.assign(size_t(emap_.ns * emap_.nk), cf(0, 0)); frW_.assign(frY_.size(), 0.0f);
        for (auto& sfr : sf_) sfr.have = false;
        return;
    }
    if (f < 0) return;
    if (f == 0) for (auto& sfr : sf_) sfr.have = false;
    sf_[f].have = true; sf_[f].Y = frY_; sf_[f].W = frW_; sf_[f].fac = fac; sf_[f].facOk = ok;
    if (f == 2 && sf_[0].have && sf_[1].have) { superFrameDone(); for (auto& sfr : sf_) sfr.have = false; }
}

// ── SDC + MSC per super frame ──────────────────────────────────────────────────────────────────────────────────────

void Receiver::superFrameDone() {
    if (!haveFac_) return;
    const Fac& fac = sf_[0].facOk ? sf_[0].fac : lastFac_;
    if (!occupancySupported(mode_, fac.so) || fac.so != estSo_) return;
    ++st_.superFrames;
    const CellMap& m = emap_;
    auto cellsOf = [&](uint8_t type, int frameLo, int frameHi, std::vector<cf>& y, std::vector<float>& w) {
        y.clear(); w.clear();
        for (int f = frameLo; f <= frameHi; ++f)
            for (int s = 0; s < m.ns; ++s)
                for (int k = m.kmin; k <= m.kmax; ++k)
                    if (m.type[size_t(m.idx(f, s, k))] == type) {
                        const size_t i = size_t(s * m.nk + (k - m.kmin));
                        y.push_back(sf_[f].Y[i]); w.push_back(sf_[f].W[i]);
                    }
    };
    // SDC
    std::vector<cf> y; std::vector<float> w;
    cellsOf(kSdc, 0, 0, y, w);
    MlcLayout sdcL;
    if (sdcL.buildSdc(fac.sdcLevels(), m.nSdc) && int(y.size()) == m.nSdc) {
        st_.sdcRate = double(sdcL.l1 + sdcL.l2) / (2.0 * (sdcL.n1 + sdcL.n2) * sdcL.levels);
        {   // ★ the SDC's MER, as the MSC's: ~1000 cells once per super frame, cheap
            double ps = 0, pe = 0;
            for (const cf& v : y) { const cf d = nearestQam(v, fac.sdcLevels()); ps += std::norm(d); pe += std::norm(v - d); }
            const float mer = pe > 0 ? float(10.0 * std::log10(ps / pe)) : 40.0f;
            sdcMerAcc_ = sdcMerN_++ == 0 ? mer : 0.7f * sdcMerAcc_ + 0.3f * mer;
            st_.sdcMerDb = sdcMerAcc_;
        }
        std::vector<uint8_t> u = mlc_.decode(y.data(), w.data(), sdcL, 2);
        prbs(u.data(), u.size());
        Sdc next = st_.sdc;
        if (next.parse(u.data(), u.size(), sdcDataBytesTable(mode_, fac.sdcMode, fac.so))) {
            st_.sdc = next; st_.sdcOk = true; ++st_.sdcGood; markLock(4);
            st_.protA = next.protA; st_.protB = next.protB;
        } else { st_.sdcOk = false; ++st_.sdcBad; }
    }
    // MSC
    st_.mscOk = false;
    if (!st_.sdc.haveMux || st_.sdc.streams.empty()) { if (onSuperFrame) onSuperFrame(); return; }
    int sumA = 0, sumB = 0;
    for (const auto& sd : st_.sdc.streams) { sumA += sd.lenA; sumB += sd.lenB; }
    MlcLayout L;
    if (!L.buildMsc(fac.mscLevels(), m.nMux, st_.sdc.protA, st_.sdc.protB, sumA) || (sumA + sumB) * 8 > L.l1 + L.l2) {
        if (onSuperFrame) onSuperFrame(); return;
    }
    {
        Rate r[3]; int lcm; mscRates(fac.mscLevels(), st_.sdc.protB, r, lcm);
        double rs = 0; for (int p = 0; p < fac.mscLevels(); ++p) rs += double(r[p].rx) / r[p].ry;
        st_.codeRateB = rs / fac.mscLevels();
        st_.codeRateA = 0;
        if (sumA > 0 && mscRates(fac.mscLevels(), st_.sdc.protA, r, lcm)) {
            double ra = 0; for (int p = 0; p < fac.mscLevels(); ++p) ra += double(r[p].rx) / r[p].ry;
            st_.codeRateA = ra / fac.mscLevels();
        }
    }
    if (cdi_.nMux() != m.nMux || cdi_.depth() != fac.interleaveDepth()) cdi_.reset(m.nMux, fac.interleaveDepth());
    cellsOf(kMsc, 0, 2, y, w);
    if (int(y.size()) != 3 * m.nMux) { if (onSuperFrame) onSuperFrame(); return; }
    {
        double ps = 0, pe = 0;
        for (const cf& v : y) { const cf d = nearestQam(v, fac.mscLevels()); ps += std::norm(d); pe += std::norm(v - d); }
        const float mer = pe > 0 ? float(10.0 * std::log10(ps / pe)) : 40.0f;
        mscMerAcc_ = st_.superFrames <= 1 ? mer : 0.7f * mscMerAcc_ + 0.3f * mer;
        st_.mscMerDb = mscMerAcc_;
    }
    std::vector<cf> z; std::vector<float> zw;
    for (int mf = 0; mf < 3; ++mf) {
        if (!cdi_.push(y.data() + size_t(mf) * size_t(m.nMux), w.data() + size_t(mf) * size_t(m.nMux), z, zw)) continue;
        std::vector<uint8_t> u = mlc_.decode(z.data(), zw.data(), L, 2);
        prbs(u.data(), u.size());
        const std::vector<uint8_t> bytes = bitsToBytes(u.data(), size_t((sumA + sumB) * 8));
        st_.mscOk = true; markLock(5);
        int offA = 0, offB = sumA;
        for (size_t si = 0; si < st_.sdc.streams.size(); ++si) {
            const StreamDesc& sd = st_.sdc.streams[si];
            LogicalFrame lf; lf.stream = int(si); lf.lenA = sd.lenA;
            lf.bytes.insert(lf.bytes.end(), bytes.begin() + offA, bytes.begin() + offA + sd.lenA);
            lf.bytes.insert(lf.bytes.end(), bytes.begin() + offB, bytes.begin() + offB + sd.lenB);
            offA += sd.lenA; offB += sd.lenB;
            // text message of the (first) audio service carried in this stream
            for (const auto& sv : st_.sdc.svc)
                if (sv.audio.present && sv.audio.streamId == int(si)) {
                    if (sv.audio.text && lf.bytes.size() >= 4 && text_.push(lf.bytes.data() + lf.bytes.size() - 4)) st_.text = text_.text();
                    st_.bitrateBps = int((lf.bytes.size() - (sv.audio.text ? 4 : 0)) * 8 / 0.4);
                    break;
                }
            if (onLogicalFrame) onLogicalFrame(lf);
        }
    }
    if (onSuperFrame) onSuperFrame();
}

// ── ★★ V12 THE SIGNAL PANE: delay spread and Doppler from the channel estimate the equaliser already has ──────────
//
// Neither number costs the receiver anything it was not already computing: the delay profile reads the time-interpolated
// pilot columns equalise() has just built, the Doppler reads the raw gain-reference estimates already in the ring. Both
// run on the vibe-drm thread (decoder priority) — never the audio thread. Per symbol the Doppler sum is ~nk/(x·y) complex
// products (≈ 35 in mode B); every y-th symbol the delay profile is ONE P-point FFT (P ≤ 256). Measured, not a hot loop
// (docs/v12/DRM.md "CPU"), so plain C++: SIMD here would buy nothing anyone could measure.

/** ★ Per plan (frame sync, or the FAC naming a new occupancy): the Hann window over the pilot columns, the FFT, and the
 *  receiver's OWN 95 % width — the same chain fed an ideal one-path channel (H = 1). A real channel's width less this one
 *  is the channel's: the estimator's resolution (~1/bandwidth, widened by the window) is not reported as spread. */
void Receiver::delayPlan() {
    const ModeInfo& mi = modeInfo(mode_);
    const int P = tu_ / mi.x;
    dsCols_ = int(wienerCols_.size()); dsPdp_.clear(); dsN_ = 0;
    if (dsCols_ < 8) { dsFft_.reset(); return; }
    dsK0_ = emap_.kmin + wienerCols_.front();
    const int span = (emap_.kmin + wienerCols_.back() - dsK0_) / mi.x + 1;   // column positions, the unused carrier's hole included
    if (span > P) { dsFft_.reset(); return; }
    if (!dsFft_ || dsFft_->size() != P) dsFft_.reset(new vibedsp::ComplexFFT(P));
    dsIn_.assign(size_t(P), cf(0, 0)); dsOut_.assign(size_t(P), cf(0, 0));
    dsWin_.assign(size_t(span), 0.0f);
    // ★ Hann, not rectangular: a rectangle's −13 dB sidelobes would be read as echoes 3 ms either side of every path
    for (int m = 0; m < span; ++m) dsWin_[size_t(m)] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * (m + 0.5) / span));
    for (int c : wienerCols_) { const int m = (emap_.kmin + c - dsK0_) / mi.x; dsIn_[size_t(m)] = cf(dsWin_[size_t(m)], 0); }
    dsFft_->forward(dsIn_.data(), dsOut_.data());
    std::vector<float> ideal(static_cast<size_t>(P));
    for (int t = 0; t < P; ++t) ideal[size_t(t)] = std::norm(dsOut_[size_t(t)]);
    dsIdeal_ = width95(ideal, 0.0f, double(std::max_element(ideal.begin(), ideal.end()) - ideal.begin()));
}

/** ★ The power-delay profile: |Σ_m w_m·H(k0 + x·m)·e^{+j2π m t / P}|², t in samples (41.7 µs at 24 kHz), aliasing at
 *  P = Tu/x samples — ≥ 2·Tg in every mode, so anything the guard can absorb fits. conj in, |·|² out turns the forward FFT
 *  into the inverse one. Averaged over ~25 profiles (~2 s in mode B): fading moves power between paths within a second,
 *  and the spread is a property of the paths, not of one moment. */
void Receiver::delayAccumulate(const std::vector<cf>& H, const std::vector<int>& cols) {
    if (!dsFft_) return;
    const ModeInfo& mi = modeInfo(mode_);
    const int P = dsFft_->size();
    std::fill(dsIn_.begin(), dsIn_.end(), cf(0, 0));
    for (int c : cols) {
        const int m = (emap_.kmin + c - dsK0_) / mi.x;
        if (m >= 0 && m < int(dsWin_.size())) dsIn_[size_t(m)] = std::conj(H[size_t(c)]) * dsWin_[size_t(m)];
    }
    dsFft_->forward(dsIn_.data(), dsOut_.data());
    if (int(dsPdp_.size()) != P) { dsPdp_.assign(size_t(P), 0.0f); dsN_ = 0; }
    const float a = dsN_ < 25 ? 1.0f / float(dsN_ + 1) : 0.04f;
    for (int t = 0; t < P; ++t) dsPdp_[size_t(t)] += (std::norm(dsOut_[size_t(t)]) - dsPdp_[size_t(t)]) * a;
    ++dsN_;
}

/** The shortest window (samples, fractional) holding 95 % of the profile's energy above `floor`. Bins under −30 dB of the
 *  peak count as nothing: the window's far sidelobes and what is left of the noise.
 *  ★★ NOT CIRCULAR: the profile wraps every P = Tu/x samples, so two paths can be read either way round (in mode D, 5 ms
 *     apart or 4.3 ms the other way). The timing loop has put the paths' centre at `centre`, so the profile is unwrapped
 *     half a span either side of it — the reading in which the paths sit where the guard interval holds them. */
float Receiver::width95(const std::vector<float>& pdp, float floor, double centre) {
    const int P = int(pdp.size());
    if (P == 0) return 0;
    const float pk = *std::max_element(pdp.begin(), pdp.end()) - floor;
    if (!(pk > 0)) return 0;
    const int start = ((int(std::lround(centre)) + P / 2) % P + P) % P;
    std::vector<float> q(static_cast<size_t>(P));
    double T = 0;
    for (int i = 0; i < P; ++i) { float v = pdp[size_t((start + i) % P)] - floor; if (v < pk * 1e-3f) v = 0; q[size_t(i)] = v; T += v; }
    const double need = 0.95 * T;
    double best = P, sum = 0;
    int e = 0;
    for (int s = 0; s < P; ++s) {
        while (sum < need && e < P) { sum += q[size_t(e)]; ++e; }
        if (sum < need) break;
        const double last = q[size_t(e - 1)];
        best = std::min(best, double(e - s) - (last > 0 ? (sum - need) / last : 0.0));
        sum -= q[size_t(s)];
    }
    return float(best);
}

/** ★★ DOPPLER from the gain references' decorrelation in time. A carrier's gain reference recurs every y symbols, so the
 *  ring holds the same carrier's raw estimate at lags τ = y·Ts and 2τ. Summed over the symbol's references (normalised by
 *  their power) and averaged over ~3 s, R(τ) = Σ H(t)·H*(t−τ). For a Gaussian Doppler spectrum of standard deviation σ
 *  (annex B's channels; Doppler spread 2σ), |R(τ)| = P·exp(−2π²σ²τ²); white noise adds to R(0) only, so the RATIO of two
 *  non-zero lags, |R(2τ)| / |R(τ)| = exp(−6π²σ²τ²), is free of the noise that would bias a lag-0 normalisation. */
void Receiver::dopplerAccumulate(const Sym& now) {
    const ModeInfo& mi = modeInfo(mode_);
    const int n = int(ring_.size());
    if (n < 2 * mi.y + 1) return;
    const Sym& a = ring_[size_t(n - 1 - mi.y)];
    const Sym& b = ring_[size_t(n - 1 - 2 * mi.y)];
    if (a.g != now.g - mi.y || b.g != now.g - 2 * mi.y) return;
    cf c1(0, 0), c2(0, 0); double e = 0; int cnt = 0;
    const size_t nk = std::min({ now.Hp.size(), a.Hp.size(), b.Hp.size() });
    for (size_t j = 0; j < nk; ++j) {
        if (!now.hasP[j] || !a.hasP[j] || !b.hasP[j]) continue;
        c1 += now.Hp[j] * std::conj(a.Hp[j]);
        c2 += now.Hp[j] * std::conj(b.Hp[j]);
        e += std::norm(now.Hp[j]); ++cnt;
    }
    if (cnt < 4 || !(e > 0)) return;
    const float al = dopN_ < 50 ? 1.0f / float(dopN_ + 1) : float(ts_) / (3.0f * kFs);
    dop1_ += (c1 / float(e) - dop1_) * al;
    dop2_ += (c2 / float(e) - dop2_) * al;
    ++dopN_;
}

void Receiver::channelStats() {
    if (mode_ == Mode::None) return;
    const ModeInfo& mi = modeInfo(mode_);
    if (dsN_ >= 8 && !dsPdp_.empty()) {
        // ★ the noise floor is the profile's median: the paths occupy a few ms of a ≥ 6 ms span, noise all of it
        std::vector<float> tmp = dsPdp_;
        std::nth_element(tmp.begin(), tmp.begin() + long(tmp.size() / 2), tmp.end());
        const float w = width95(dsPdp_, tmp[tmp.size() / 2], tauAvg_);
        if (getenv("VIBE_DRM_DEBUG3")) {
            const float pk = *std::max_element(dsPdp_.begin(), dsPdp_.end());
            fprintf(stderr, "[drm] pdp (w %.1f ideal %.1f):", w, dsIdeal_);
            for (size_t t = 0; t < dsPdp_.size(); ++t) { const float d = 10 * std::log10(std::max(dsPdp_[t], 1e-20f) / pk); if (d > -35) fprintf(stderr, " %zu:%.0f", t, d); }
            fprintf(stderr, "\n");
        }
        st_.delayMs = std::max(0.0f, w - dsIdeal_) * 1000.0f / float(kFs);
    }
    if (dopN_ >= 50) {
        const double r1 = std::abs(dop1_), r2 = std::abs(dop2_);
        if (r1 > 1e-9) {
            // ★ a ratio under 0.02 is fading too fast for these lags to measure: it reads as the largest they can tell
            const double r = std::min(1.0, std::max(0.02, r2 / r1));
            const double tau = double(mi.y) * ts_ / kFs;
            const double sig2 = -std::log(r) / (6.0 * M_PI * M_PI * tau * tau);
            st_.dopplerHz = float(2.0 * std::sqrt(std::max(0.0, sig2)));
        }
    }
}

}  // namespace vibedrm
