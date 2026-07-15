// VibeSDR V5 — DDC: NCO (complex mixer) + windowed-sinc low-pass + decimator.
// Original VibeSDR code. The FIR inner loops and the NCO are the per-sample hot
// paths (they run at the full IQ rate), so they are SIMD-friendly + trig-free.
#include "vibedsp.h"
#include "simd_internal.h"   // dotReal/dotCplx (NEON), fastAtan2
#include <cmath>
#include <algorithm>

namespace vibedsp {

// ── NCO ──────────────────────────────────────────────────────────────────--
void NCO::setFreq(double normFreq) {
    const double step = 2.0 * M_PI * normFreq;     // radians/sample
    rot_ = cf32((float)std::cos(step), (float)-std::sin(step));  // exp(-j*step)
}

void NCO::mix(const cf32* in, cf32* out, int n) {
    cf32 cur = cur_;
    const cf32 rot = rot_;
    for (int i = 0; i < n; ++i) {
        out[i] = in[i] * cur;     // multiply by running phasor (no per-sample trig)
        cur *= rot;
        // Renormalise occasionally so |cur| doesn't drift from 1 (recursion error).
        if (++sinceNorm_ >= 1024) {
            sinceNorm_ = 0;
            const float m = 1.0f / std::sqrt(cur.real() * cur.real() + cur.imag() * cur.imag());
            cur *= m;
        }
    }
    cur_ = cur;
}

// ── FIR low-pass design (windowed sinc, Hamming) ─────────────────────────--
std::vector<float> designLowpass(double cutoff, double transition, bool deepStop) {
    // Tap count from transition width (Hamming ~ 3.3/BW, Blackman ~ 5.5/BW), odd for
    // symmetry. Blackman buys ~74 dB of stopband against Hamming's ~53 dB — which
    // matters wherever what leaks through will FOLD into the audio and can't be
    // removed later.
    int n = (int)std::ceil((deepStop ? 5.5 : 3.3) / std::max(transition, 1e-4));
    if ((n & 1) == 0) ++n;
    if (n < 9) n = 9;
    std::vector<float> h(n);
    const double mid = (n - 1) / 2.0;
    const double wc = 2.0 * M_PI * cutoff;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        const double k = i - mid;
        double sinc = (k == 0.0) ? (wc / M_PI) : std::sin(wc * k) / (M_PI * k);
        const double t = 2.0 * M_PI * i / (n - 1);
        const double win = deepStop
            ? (0.42 - 0.5 * std::cos(t) + 0.08 * std::cos(2.0 * t))   // Blackman ~74 dB
            : (0.54 - 0.46 * std::cos(t));                            // Hamming  ~53 dB
        h[i] = (float)(sinc * win);
        sum += h[i];
    }
    for (auto& v : h) v /= (float)sum;   // unity DC gain
    return h;
}

// ── FIR decimator (complex stream, real taps) ────────────────────────────--
FirDecimator::FirDecimator(std::vector<float> taps, int decim)
    : decim_(decim), phase_(decim), K_((int)taps.size()) {
    rtaps_.assign(taps.rbegin(), taps.rend());   // reversed for contiguous dot
    buf_.assign(K_ - 1, cf32(0.0f, 0.0f));        // K-1 sample history
}

void FirDecimator::reset() {
    std::fill(buf_.begin(), buf_.end(), cf32(0.0f, 0.0f));
    buf_.resize(K_ - 1);
    phase_ = decim_;
}

int FirDecimator::process(const cf32* in, int n, cf32* out) {
    // buf_ = [K-1 history][block]; y[i] = dot(rtaps_, &buf_[i]) over K samples.
    buf_.resize((size_t)(K_ - 1 + n));
    std::copy(in, in + n, buf_.begin() + (K_ - 1));
    const float* z = reinterpret_cast<const float*>(buf_.data());  // interleaved re/im
    int outn = 0;
    for (int i = 0; i < n; ++i) {
        if (--phase_ == 0) {
            phase_ = decim_;
            out[outn++] = dotCplx(rtaps_.data(), z + 2 * i, K_);
        }
    }
    // Carry the last K-1 samples as the next call's history.
    std::copy(buf_.end() - (K_ - 1), buf_.end(), buf_.begin());
    buf_.resize(K_ - 1);
    return outn;
}

// ── AM demod ─────────────────────────────────────────────────────────────--
void AmDemod::process(const cf32* in, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        const float mag = std::sqrt(in[i].real() * in[i].real() +
                                    in[i].imag() * in[i].imag());
        // One-pole DC blocker removes the carrier term, leaving the modulation.
        dc_ = kPole * dc_ + (1.0f - kPole) * mag;
        out[i] = (mag - dc_) * kGain;
    }
}

// ── FM demod (quadrature discriminator) ──────────────────────────────────--
void FmDemod::process(const cf32* in, float* out, int n) {
    // Accurate fast atan2 (~1e-6 error): the discriminator feeds the 23-53 kHz MPX
    // and thus the L-R stereo difference, so it must stay clean — the earlier crude
    // approximation (~3.8e-3) audibly corrupted stereo. This minimax version is
    // inaudible and far cheaper than std::atan2 at the channel rate.
    for (int i = 0; i < n; ++i) {
        const cf32 d = in[i] * std::conj(prev_);
        out[i] = gain_ * fastAtan2(d.imag(), d.real());
        prev_ = in[i];
    }
}

// ── SSB / CW demod (Weaver / third method) ───────────────────────────────--
void SsbDemod::configure(Side side, double bwHz, double rate) {
    side_ = side;
    const double fc = bwHz * 0.5;              // sub-carrier = half the SSB bandwidth
    // SIGNED: USB centres the upper sideband (mix down by +fc), LSB centres the
    // lower (mix down by -fc = up). Both mix stages must flip together or the
    // sidebands swap and the image isn't rejected (it appears frequency-mirrored).
    const double w = (side == Side::LSB ? -1.0 : 1.0) * 2.0 * M_PI * fc / rate;
    rot_ = cf32((float)std::cos(w), (float)std::sin(w));   // e^{j*w} per sample
    cur_ = cf32(1.0f, 0.0f); sinceNorm_ = 0;
    // Low-pass at bw/2 (normalised): keeps the centred wanted sideband, rejects
    // the image (which the down-mix pushed just past +/- bw/2). The wanted and
    // unwanted sidebands MEET at the carrier (= +/- bw/2 after centring), so the
    // filter's transition band straddles them — a wide transition lets the wrong
    // sideband's low-audio content bleed through. Use a SHARP transition (~80 Hz)
    // so only sub-~80 Hz of the wrong sideband leaks (inaudible); the FIRs are
    // NEON-accelerated so the longer filter is cheap.
    const double cutoff = fc / rate;
    const double trans  = std::max(cutoff * 0.06, 80.0 / rate);
    std::vector<float> taps = designLowpass(cutoff, trans);
    lpfI_ = std::make_unique<RealFir>(taps);
    lpfQ_ = std::make_unique<RealFir>(taps);
}

void SsbDemod::reset() {
    cur_ = cf32(1.0f, 0.0f); sinceNorm_ = 0;
    if (lpfI_) lpfI_->reset();
    if (lpfQ_) lpfQ_->reset();
}

void SsbDemod::process(const cf32* in, float* out, int n) {
    aI_.resize(n); aQ_.resize(n); cbuf_.resize(n); sbuf_.resize(n);
    // Mix by signed fc via a recursive rotator (c,s = cur_ advanced by rot_ each
    // sample — no per-sample trig): a = z * e^{-j*phase} = z * (cos - j sin). The
    // sideband sign lives in rot_ (down by +fc for USB, up by fc for LSB).
    for (int i = 0; i < n; ++i) {
        const float c = cur_.real(), s = cur_.imag();
        cbuf_[i] = c; sbuf_[i] = s;
        const float zr = in[i].real(), zi = in[i].imag();
        aI_[i] = zr * c + zi * s;
        aQ_[i] = zi * c - zr * s;
        cur_ *= rot_;
        if (++sinceNorm_ >= 1024) {           // renormalise to fight magnitude drift
            sinceNorm_ = 0;
            const float m = 1.0f / std::sqrt(cur_.real()*cur_.real() + cur_.imag()*cur_.imag());
            cur_ *= m;
        }
    }
    fI_.resize(n); fQ_.resize(n);
    lpfI_->process(aI_.data(), n, fI_.data());
    lpfQ_->process(aQ_.data(), n, fQ_.data());
    // Mix back and take real: out = Re{aLPF * e^{+j*phase}} = fI*c - fQ*s. The
    // sideband sign already lives in phase, so the formula is the same for both.
    for (int i = 0; i < n; ++i) out[i] = fI_[i] * cbuf_[i] - fQ_[i] * sbuf_[i];
}

// ── Real FIR (low-pass / optional decimate) ──────────────────────────────--
RealFir::RealFir(std::vector<float> taps, int decim)
    : decim_(decim), phase_(decim), K_((int)taps.size()) {
    rtaps_.assign(taps.rbegin(), taps.rend());   // reversed for contiguous dot
    buf_.assign(K_ - 1, 0.0f);                     // K-1 sample history
}

void RealFir::reset() {
    std::fill(buf_.begin(), buf_.end(), 0.0f);
    buf_.resize(K_ - 1);
    phase_ = decim_;
}

int RealFir::process(const float* in, int n, float* out) {
    // buf_ = [K-1 history][block]; y[i] = dot(rtaps_, &buf_[i]) over K samples.
    buf_.resize((size_t)(K_ - 1 + n));
    std::copy(in, in + n, buf_.begin() + (K_ - 1));
    int outn = 0;
    for (int i = 0; i < n; ++i) {
        if (--phase_ == 0) {
            phase_ = decim_;
            out[outn++] = dotReal(rtaps_.data(), buf_.data() + i, K_);
        }
    }
    std::copy(buf_.end() - (K_ - 1), buf_.end(), buf_.begin());
    buf_.resize(K_ - 1);
    return outn;
}

} // namespace vibedsp
