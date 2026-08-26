// VibeSDR V4 — MMSE spectral-weighting audio NR (Kim & Ruwisch / Ephraim-Malah).
#include "audio_nr.h"
#include <cmath>
#include <algorithm>

namespace vibe {

// Fixed algorithm constants (from the KiwiSDR "spectral NR" reference).
static const float kPsthr  = 0.99f;   // smoothed speech-probability threshold
static const float kPnsaf  = 0.01f;   // noise-probability safety value
static const float kPsini  = 0.5f;    // initial speech probability
static const float kPspri  = 0.5f;    // prior speech probability
static const float kAlpha  = 0.95f;   // decision-directed a-priori SNR smoothing
static const float kAx     = 0.8f;    // noise PSD smoothing (per 21 ms frame)
static const float kAp     = 0.9f;    // speech-prob smoothing
static const float kAsnrDb = 30.0f;   // assumed a-priori SNR when speech present
static const float kSnrPostMax = 1000.0f;
static const float kSnrPrioMin = 0.001f;   // -30 dB
static const int   kNRwidth = 4;      // musical-noise cross-bin average half-width
static const int   kInitFrames = 16;  // ~0.34 s of noise priming after a reset

AudioNR::AudioNR() {
    fwd = kiss_fftr_alloc(N, 0, nullptr, nullptr);
    inv = kiss_fftr_alloc(N, 1, nullptr, nullptr);
    win.resize(N);
    for (int i = 0; i < N; i++) {
        float h = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (N - 1)));
        win[i] = std::sqrt(h);                 // sqrt-Hann: COLA at 50% overlap
    }
    ola.assign(N, 0.0f);
    frame.resize(N);
    spec.resize(K);
    pw.assign(K, 0.0f);
    xt.assign(K, 0.0f);
    pslp.assign(K, kPsini);
    hkOld.assign(K, 1.0f);
    gain.assign(K, 1.0f);
    gtmp.assign(K, 1.0f);

    // a-priori-SNR (H1) constants — xih1 = 10^(asnr_dB/10).
    xih1  = std::pow(10.0f, kAsnrDb / 10.0f);
    xih1r = 1.0f / (1.0f + xih1) - 1.0f;
    pfac  = (1.0f / kPspri - 1.0f) * (1.0f + xih1);
    reset();
}

AudioNR::~AudioNR() {
    if (fwd) kiss_fftr_free(fwd);
    if (inv) kiss_fftr_free(inv);
}

void AudioNR::reset() {
    inBuf.clear();
    std::fill(ola.begin(), ola.end(), 0.0f);
    std::fill(xt.begin(), xt.end(), 0.0f);
    std::fill(pslp.begin(), pslp.end(), kPsini);
    std::fill(hkOld.begin(), hkOld.end(), 1.0f);
    std::fill(gain.begin(), gain.end(), 1.0f);
    initStage = 1;     // prime the noise estimate before reducing
    initCount = 0;
    primed = false;
}

void AudioNR::process(const float* in, int count, std::vector<float>& out) {
    inBuf.insert(inBuf.end(), in, in + count);
    const float invN = 1.0f / (float)N;
    /* ★★★ HOW MUCH, NOT JUST HOW DEEP. `strength` used to set only the FLOOR, so the suppression
     *     RULE below ran at full force at every setting and the bottom of the slider was already
     *     hard NR — "even setting 1 is very aggressive" (Stuart, 2026-08-26). There was never a
     *     gentler end, only a shallower one, which is why the control never felt granular.
     *  ★★ `mix` is a dry/wet blend applied to the GAIN (step 4), not to the audio: the wet path is
     *     an STFT with overlap-add and therefore DELAYED, so blending dry audio into it would need
     *     a matched delay line and would comb-filter the moment it drifted. Blending toward unity
     *     gain instead — `1 - mix*(1 - g)` — is the same thing done where both signals are already
     *     aligned by construction, on the same frame, before the inverse FFT. mix=0 is a literal
     *     passthrough (every gain becomes exactly 1.0); mix=1 is the full suppressor as before.
     *  ★ The ESTIMATOR never sees the blend. `hkOld` keeps taking the raw `g` below, so the
     *    decision-directed SNR recursion stays consistent no matter where the slider sits —
     *    otherwise a low setting would corrupt the noise tracking and the high end with it. */
    // ★ Full wet by 0.7, not 1.0. If the blend only completed at the top, the floor became
    //   unconstrained in a single step and the last-but-one notch jumped ~27 dB. Finishing the
    //   blend early hands the top third over to the floor sweep, so the travel stays monotone.
    const float mix  = std::min(1.0f, strength / 0.7f);
    /* Strength 0..1 → residual-noise floor: gentle (-14 dB) … deep (-60 dB).
     * ★★ SWEPT IN dB, NOT IN AMPLITUDE. Linear amplitude (`0.2 - 0.199*s`) is nearly flat for most
     *    of the travel and then falls off a cliff at the very top — -14, -16, -20, -29, then -60.
     *    Hearing works in dB, so a control that does not is not granular however many steps it has.
     *    The exponential gives an even ~9 dB per fifth of the slider across the whole range. */
    const float gmin = std::max(0.001f, 0.2f * std::exp(strength * -3.912f));   // ln(0.005)
    // Strength >1 (slider 16..20) → over-subtraction: scale the noise PSD used for
    // the gain SNR (not the tracker) so weak-SNR bins are suppressed harder.
    const float osf = 1.0f + 1.5f * std::max(0.0f, strength - 1.0f);
    const int   lo = 1, hi = K - 1;

    while ((int)inBuf.size() >= N) {
        // Analysis: window then forward real-FFT.
        for (int i = 0; i < N; i++) frame[i] = inBuf[i] * win[i];
        kiss_fftr(fwd, frame.data(), spec.data());
        for (int k = 0; k < K; k++) pw[k] = spec[k].r * spec[k].r + spec[k].i * spec[k].i;

        if (initStage == 1) {
            // Prime the noise PSD with a short running average (no reduction yet).
            for (int k = 0; k < K; k++) xt[k] += pw[k] / (float)kInitFrames;
            if (++initCount >= kInitFrames) {
                for (int k = 0; k < K; k++) xt[k] *= kPsini;
                initStage = 2;
            }
            // gain stays 1.0 — pass through during priming.
        } else {
            // 1) MMSE noise-PSD update via per-bin speech-presence probability.
            for (int k = lo; k <= hi; k++) {
                float xtk = xt[k] > 1e-12f ? xt[k] : 1e-12f;
                float ph1y = 1.0f / (1.0f + pfac * std::exp(xih1r * pw[k] / xtk));
                pslp[k] = kAp * pslp[k] + (1.0f - kAp) * ph1y;
                if (pslp[k] > kPsthr) ph1y = 1.0f - kPnsaf;
                else                  ph1y = std::min(ph1y, 1.0f);
                float xtr = (1.0f - ph1y) * pw[k] + ph1y * xtk;
                xt[k] = kAx * xtk + (1.0f - kAx) * xtr;
            }
            // 2) a-posteriori + decision-directed a-priori SNR → MMSE-STSA gain.
            for (int k = lo; k <= hi; k++) {
                float xtk = (xt[k] > 1e-12f ? xt[k] : 1e-12f) * osf;
                float post = std::max(std::min(pw[k] / xtk, kSnrPostMax), kSnrPrioMin);
                float prio = std::max(kAlpha * hkOld[k]
                                      + (1.0f - kAlpha) * std::max(post - 1.0f, 0.0f), 0.0f);
                float v = prio * post / (1.0f + prio);
                float g = std::max((1.0f / post) * std::sqrt(0.7212f * v + v * v), gmin);
                gain[k] = g;
                hkOld[k] = post * g * g;
            }
            // 3) Musical-noise reduction: dynamically average the gains across bins
            //    when the band-wide reduction is deep (low post/pre power ratio).
            float pre = 0.0f, postP = 0.0f;
            for (int k = lo; k <= hi; k++) { pre += pw[k]; postP += gain[k] * gain[k] * pw[k]; }
            float ratio = pre > 0.0f ? postP / pre : 1.0f;
            const float pth = 0.4f;
            int NN = (ratio > pth) ? 1
                                   : 1 + 2 * (int)(0.5f + kNRwidth * (1.0f - ratio / pth));
            if (NN > 1) {
                int h2 = NN / 2;
                for (int k = lo; k <= hi; k++) {
                    int a = std::max(lo, k - h2), b = std::min(hi, k + h2);
                    float s = 0.0f; for (int m = a; m <= b; m++) s += gain[m];
                    gtmp[k] = s / (float)(b - a + 1);
                }
                for (int k = lo; k <= hi; k++) gain[k] = gtmp[k];
            }
            gain[0] = gain[lo];
            // 4) Apply the spectral weighting, blended toward unity by `mix` — see the note above.
            for (int k = 0; k < K; k++) {
                const float ge = 1.0f - mix * (1.0f - gain[k]);
                spec[k].r *= ge; spec[k].i *= ge;
            }
        }

        kiss_fftri(inv, spec.data(), frame.data());

        // Synthesis window + overlap-add; emit HOP samples once the OLA is primed.
        for (int i = 0; i < N; i++) ola[i] += frame[i] * invN * win[i];
        if (primed) {
            for (int i = 0; i < HOP; i++) out.push_back(ola[i]);
        }
        primed = true;
        for (int i = 0; i < N - HOP; i++) ola[i] = ola[i + HOP];
        for (int i = N - HOP; i < N; i++) ola[i] = 0.0f;
        inBuf.erase(inBuf.begin(), inBuf.begin() + HOP);
    }
}

} // namespace vibe
