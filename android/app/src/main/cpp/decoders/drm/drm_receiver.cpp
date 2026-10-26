// decoders/drm/drm_receiver.cpp — see drm_receiver.h for the chain and the reasons.
#include "drm_receiver.h"
#include "vibedsp.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

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
    blankFrom_ = 0; pAvg_ = 0; blankHold_ = 0; blankRun_ = 0; blanked_ = 0;
    loseSync("reset");
    st_ = RxStatus();
    text_ = TextMessageDecoder();
}

void Receiver::loseSync(const char* why) {
    if (getenv("VIBE_DRM_DEBUG") && state_ != State::Search) fprintf(stderr, "[drm] lose sync: %s\n", why);
    state_ = State::Search;
    mode_ = Mode::None; fft_.reset();
    freq_ = 0; ncoPhase_ = 0; ncoIdx_ = 0;
    havePrev_ = false; prevStep_ = 0; tauAvg_ = 0; tauN_ = 0; stepSum_ = 0; pendingStep_ = 0;
    tScore_.clear(); symCount_ = 0; frameSynced_ = false; sIdx_ = 0; gIdx_ = 0;
    ring_.clear(); frS_ = 0; frY_.clear(); frW_.clear();
    for (auto& f : sf_) f.have = false;
    lastFrameIdx_ = -1; facFailRun_ = 0; facGoodRun_ = 0; haveFac_ = false;
    merInit_ = false;
    const Sdc keepSdc;   // ★ a lost signal forgets its station: nothing stale survives a resync
    st_.timeSync = st_.freqSync = st_.frameSync = st_.facOk = st_.sdcOk = st_.mscOk = st_.locked = false;
    st_.mode = Mode::None; st_.so = -1; st_.sdc = keepSdc; st_.text.clear(); st_.bitrateBps = 0;
    text_ = TextMessageDecoder();
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
 *    the mean is taken from the new level. */
void Receiver::blankNew() {
    for (int64_t i = std::max(blankFrom_, bufBase_); i < bufEnd(); ++i) {
        cf& v = buf_[size_t(i - bufBase_)];
        const float p = std::norm(v);
        if (pAvg_ <= 0) pAvg_ = p + 1e-12f;
        // ★ the decision is on an 8-sample average: a pulse is a RUN of high samples, an OFDM peak is one or two
        //   (8 samples ≈ 16 degrees of freedom: 6 dB over the mean by chance is ~1e-6, so the threshold can sit low
        //   enough to catch a pulse's skirts, which a single-sample test at 13 dB let through)
        blankWin_[blankWi_ & 7] = p; ++blankWi_;
        float s8 = 0; for (float q : blankWin_) s8 += q;
        if (s8 > 4.0f * 8.0f * pAvg_ || p > 20.0f * pAvg_) {
            ++blankRun_;
            if (blankRun_ > kFs / 10) { pAvg_ = s8 / 8.0f; blankRun_ = 0; continue; }
            for (int64_t j = std::max(bufBase_, i - 8); j < i; ++j) buf_[size_t(j - bufBase_)] = cf(0, 0);
            blankHold_ = 8;
        }
        if (blankHold_ > 0) { --blankHold_; v = cf(0, 0); ++blanked_; continue; }
        blankRun_ = 0;
        pAvg_ += (p - pAvg_) * (1.0f / 2400.0f);
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
    const int margin = tg_ / 4;
    int64_t w = bufBase_ + best[bestM].r + tg_ - margin;
    while (w - ts_ >= bufBase_) w -= ts_;
    winPos_ = w;
    ncoPhase_ = 0; ncoIdx_ = winPos_;
    st_.timeSync = true; st_.mode = mode_;
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
    st_.freqSync = true;
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
            freq_ += 0.05 * ferr;
            st_.freqOffsetHz = freq_;
        }
        prevSym_ = car; havePrev_ = true; prevStep_ = 0;

        if (!frameSynced_) {
            // time-reference score for this position, after removing the window margin's phase slope
            const auto& tr = timeRefs(mode_);
            const int margin = tg_ / 4;
            cf s(0, 0); double nrm = 0;
            for (size_t i = 0; i + 1 < tr.size(); ++i) {
                const int ka = tr[i].k, kb = tr[i + 1].k;
                const cf xa = car[size_t(ka + kw_)] * std::polar(1.0f, float(2.0 * M_PI * ka * margin / tu_));
                const cf xb = car[size_t(kb + kw_)] * std::polar(1.0f, float(2.0 * M_PI * kb * margin / tu_));
                const cf ref = phasor(tr[i + 1].ph, 1.0f) * std::conj(phasor(tr[i].ph, 1.0f));
                s += xb * std::conj(xa) * std::conj(ref);
                nrm += std::abs(xa) * std::abs(xb);
            }
            tScore_[size_t(symCount_ % mi.ns)] += nrm > 0 ? std::abs(s) / nrm : 0;
            ++symCount_;
            if (symCount_ >= 2 * mi.ns && symCount_ % mi.ns == 0) {
                const int b = int(std::max_element(tScore_.begin(), tScore_.end()) - tScore_.begin());
                double others = 0; for (int i = 0; i < mi.ns; ++i) if (i != b) others += tScore_[size_t(i)];
                others /= (mi.ns - 1);
                if (tScore_[size_t(b)] > 3.0 * others) {
                    frameSynced_ = true;
                    // the NEXT symbol (index symCount_) has local position symCount_ % ns; s = that − b
                    sIdx_ = ((symCount_ - b) % mi.ns + mi.ns) % mi.ns;
                    gIdx_ = int64_t(mi.ns) * 1000 + sIdx_;
                    st_.frameSync = true;
                    estSo_ = 3; emap_.build(mode_, estSo_);
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

void Receiver::applyStepToRing(int delta) {
    // a window moved later by δ multiplies every carrier by e^{+j2πkδ/Tu}: bring the stored pilots along
    for (Sym& sy : ring_)
        for (int j = 0; j < emap_.nk; ++j) {
            const int k = emap_.kmin + j;
            const cf r = std::polar(1.0f, float(2.0 * M_PI * k * delta / tu_));
            sy.Hp[size_t(j)] *= r; sy.X[size_t(j)] *= r;
        }
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
    while (int(ring_.size()) > 2 * mi.y + 2) ring_.pop_front();
    // the symbol y−1 back now has its next pilots
    const int64_t gOut = gIdx_ - (mi.y - 1);
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
        if (d == 0 && hp) H[size_t(j)] = ring_[size_t(gp - g0)].Hp[size_t(j)];
        else if (hp && hn) {
            const float a = float(d) / mi.y;
            H[size_t(j)] = ring_[size_t(gp - g0)].Hp[size_t(j)] * (1.0f - a) + ring_[size_t(gn - g0)].Hp[size_t(j)] * a;
        } else if (hp) H[size_t(j)] = ring_[size_t(gp - g0)].Hp[size_t(j)];
        else if (hn) H[size_t(j)] = ring_[size_t(gn - g0)].Hp[size_t(j)];
        else continue;
        cols.push_back(j);
    }
    if (cols.size() < 2) { Y.assign(size_t(nk), cf(0, 0)); W.assign(size_t(nk), 0.0f); return; }
    // phase slope across the band (the window offset + mean delay)
    cf S(0, 0);
    for (size_t i = 0; i + 1 < cols.size(); ++i)
        if (cols[i + 1] - cols[i] == mi.x) S += H[size_t(cols[i + 1])] * std::conj(H[size_t(cols[i])]);
    const float theta = std::arg(S) / float(mi.x);
    // derotate, interpolate between columns, rotate back
    std::vector<cf> Hd(static_cast<size_t>(nk));
    for (int c : cols) Hd[size_t(c)] = H[size_t(c)] * std::polar(1.0f, -theta * float(emap_.kmin + c));
    for (size_t i = 0; i + 1 < cols.size(); ++i) {
        const int a = cols[i], b = cols[i + 1];
        for (int j = a + 1; j < b; ++j) { const float t = float(j - a) / float(b - a); Hd[size_t(j)] = Hd[size_t(a)] * (1.0f - t) + Hd[size_t(b)] * t; }
    }
    for (int j = 0; j < cols.front(); ++j) Hd[size_t(j)] = Hd[size_t(cols.front())];
    for (int j = cols.back() + 1; j < nk; ++j) Hd[size_t(j)] = Hd[size_t(cols.back())];
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
    // timing: τ = −θ·Tu/2π, held at the window margin
    const double tau = -double(theta) * tu_ / (2.0 * M_PI);
    tauAvg_ = tauN_ == 0 ? tau : 0.95 * tauAvg_ + 0.05 * tau; ++tauN_;
    const double target = tg_ / 4;
    if (tauN_ > 8 && pendingStep_ == 0) {
        if (tauAvg_ > target + 1.0) { pendingStep_ = +1; tauAvg_ -= 1.0; applyStepToRing(+1); }
        else if (tauAvg_ < target - 1.0) { pendingStep_ = -1; tauAvg_ += 1.0; applyStepToRing(-1); }
    }
}

// ── FAC per frame ───────────────────────────────────────────────────────────────────────────────────────────────

void Receiver::frameDone() {
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
        facMerAcc_ = merInit_ ? 0.8f * facMerAcc_ + 0.2f * mer : mer; merInit_ = true;
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
        st_.facOk = true; st_.fac = fac;
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
        estSo_ = fac.so; emap_.build(mode_, estSo_);
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
        std::vector<uint8_t> u = mlc_.decode(y.data(), w.data(), sdcL, 2);
        prbs(u.data(), u.size());
        Sdc next = st_.sdc;
        if (next.parse(u.data(), u.size(), sdcDataBytesTable(mode_, fac.sdcMode, fac.so))) {
            st_.sdc = next; st_.sdcOk = true; ++st_.sdcGood;
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
        st_.mscOk = true;
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

}  // namespace vibedrm
