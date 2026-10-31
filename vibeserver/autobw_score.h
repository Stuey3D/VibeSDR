// autobw_score.h — the score that judges auto bandwidth (V12, docs/v12/AM-SYNC.md §11). Header-only; shared by
// vibeserver/test-auto-bw.cpp (synthetic, against truth) and tools/auto-bw/ab_replay.cpp (off air, against a second
// transmitter of the same programme).
//
// ★★★ WHY NOT THE AM SCORE. AM-SYNC.md §4 scored with magnitude-squared coherence per frequency. For USB/LSB/CW a
//     filter after the detector is LINEAR: it scales the programme and the noise at each frequency alike, so a
//     per-frequency S/N cannot move at all — that score is blind to exactly what auto bandwidth does. What a passband
//     changes is the BROADBAND picture: how much of what reaches the ear is the wanted signal, and how much of the
//     wanted signal was thrown away to get there. So:
//
//       per 2 s window, per FFT bin f (Welch, 1024-point Hann segments, 50 % overlap, 48 kHz audio):
//         C(f) = |Pxy|² / Pxx        the part of the output coherent with the reference (the wanted signal)
//         N(f) = Pyy − C(f)          everything else: noise, a neighbour, a heterodyne, distortion
//       each run normalised so Σ C over the CORE band = 1 (level- and AGC-blind), then
//         score = Σ C  /  Σ ( N + max(0, C_fixed − C) )      over the analysis band
//       i.e. "wanted ÷ (unwanted + any wanted the filter removed that the fixed passband kept)". The fixed run has no
//       loss term by definition; cutting a band that held only noise raises the score, cutting the voice lowers it.
//     Compared PAIRED, window by window, against the fixed passband on the same signal: mean ± standard error.
#pragma once
#include "vibedsp/vibedsp.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace abscore {

struct Spec { std::vector<double> pxx, pyy; std::vector<std::complex<double>> pxy; };

/** Welch spectra of one window: x = reference, y = output (same length, already aligned). */
inline Spec welch(const float* x, const float* y, int n, int seg = 1024) {
    Spec s; const int nb = seg / 2 + 1;
    s.pxx.assign(nb, 0.0); s.pyy.assign(nb, 0.0); s.pxy.assign(nb, {0.0, 0.0});
    vibedsp::RealFFT f(seg);
    std::vector<float> w(seg), bx(seg), by(seg);
    for (int i = 0; i < seg; ++i) w[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / seg));
    std::vector<vibedsp::cf32> X(nb), Y(nb);
    for (int p = 0; p + seg <= n; p += seg / 2) {
        for (int i = 0; i < seg; ++i) { bx[i] = x[p + i] * w[i]; by[i] = y[p + i] * w[i]; }
        f.forward(bx.data(), X.data()); f.forward(by.data(), Y.data());
        for (int k = 0; k < nb; ++k) {
            const std::complex<double> a(X[k].real(), X[k].imag()), b(Y[k].real(), Y[k].imag());
            s.pxx[k] += std::norm(a); s.pyy[k] += std::norm(b); s.pxy[k] += std::conj(a) * b;
        }
    }
    return s;
}

/** Coherent (C) and residual (N) power per bin, normalised so Σ C over [coreLo, coreHi) Hz = 1. */
inline void cn(const Spec& s, double fs, double coreLo, double coreHi, std::vector<double>& C, std::vector<double>& N) {
    const int nb = (int)s.pxx.size(); const double binHz = fs / (2.0 * (nb - 1));
    C.assign(nb, 0.0); N.assign(nb, 0.0);
    double k = 0.0;
    for (int b = 0; b < nb; ++b) {
        C[b] = s.pxx[b] > 0 ? std::norm(s.pxy[b]) / s.pxx[b] : 0.0;
        N[b] = std::max(0.0, s.pyy[b] - C[b]);
        const double f = b * binHz;
        if (f >= coreLo && f < coreHi) k += C[b];
    }
    if (k > 0) for (int b = 0; b < nb; ++b) { C[b] /= k; N[b] /= k; }
}

/** dB score of a run over [lo, hi) Hz, against the fixed run's C (pass Cfix = C for the fixed run itself). */
inline double score(const std::vector<double>& C, const std::vector<double>& N, const std::vector<double>& Cfix,
                    double fs, double lo, double hi, double* keptOut = nullptr) {
    const int nb = (int)C.size(); const double binHz = fs / (2.0 * (nb - 1));
    double s = 0, u = 0, kept = 0, base = 0;
    for (int b = 0; b < nb; ++b) {
        const double f = b * binHz; if (f < lo || f >= hi) continue;
        s += C[b]; u += N[b] + std::max(0.0, Cfix[b] - C[b]);
        kept += std::min(C[b], Cfix[b]); base += Cfix[b];
    }
    if (keptOut) *keptOut = base > 0 ? kept / base : 1.0;
    return 10.0 * std::log10(std::max(s, 1e-30) / std::max(u, 1e-30));
}

/** The output's delay against the reference (samples): a coarse search on every 6th sample, then refined ±6. */
inline int lagOf(const std::vector<float>& x, const std::vector<float>& y, int maxLag, size_t from, size_t len) {
    auto corr = [&](int L, int step) {
        double acc = 0;
        for (size_t i = from; i < from + len && i + L < y.size() && i < x.size(); i += step) acc += (double)x[i] * y[i + L];
        return std::fabs(acc);
    };
    int best = 0; double bv = -1;
    for (int L = 0; L <= maxLag; L += 3) { const double v = corr(L, 6); if (v > bv) { bv = v; best = L; } }
    int b2 = best; bv = -1;
    for (int L = std::max(0, best - 6); L <= best + 6; ++L) { const double v = corr(L, 1); if (v > bv) { bv = v; b2 = L; } }
    return b2;
}

/** ★★ OFF AIR, THE PHASE CANNOT BE USED. Two transmitters of one programme reach us a fraction of a hertz apart
 *  (ionospheric Doppler, synthesiser offsets), so their demodulated SSB audio drifts in phase and a 2 s Welch
 *  cross-spectrum averages to nothing (measured: waveform correlation 0.09 between Shannon VOLMET 3413 and 5505 kHz
 *  whose envelopes correlate at 0.54). So per 100 Hz band, the output's POWER is regressed on the reference's power
 *  segment by segment: P_y(t) = a·P_r(t) + n. a·mean(P_r) is the programme in that band, n the rest. Phase-free and
 *  tolerant of a few hertz of offset. Fills C and N per band (index = band), normalised like cn(). */
inline void cnPower(const float* x, const float* y, int n, double fs, double coreLo, double coreHi, double bandHz,
                    std::vector<double>& C, std::vector<double>& N, int seg = 1024) {
    const int nb = seg / 2 + 1; const double binHz = fs / seg; const int nBand = (int)(fs / 2 / bandHz);
    vibedsp::RealFFT f(seg);
    std::vector<float> w(seg), bx(seg), by(seg);
    for (int i = 0; i < seg; ++i) w[i] = (float)(0.5 - 0.5 * std::cos(2 * M_PI * i / seg));
    std::vector<vibedsp::cf32> X(nb), Y(nb);
    std::vector<std::vector<double>> pr, py;
    for (int p = 0; p + seg <= n; p += seg / 2) {
        for (int i = 0; i < seg; ++i) { bx[i] = x[p + i] * w[i]; by[i] = y[p + i] * w[i]; }
        f.forward(bx.data(), X.data()); f.forward(by.data(), Y.data());
        std::vector<double> a(nBand, 0.0), b(nBand, 0.0);
        for (int k = 0; k < nb; ++k) { const int band = (int)(k * binHz / bandHz); if (band >= nBand) break;
            a[band] += std::norm(std::complex<double>(X[k].real(), X[k].imag())); b[band] += std::norm(std::complex<double>(Y[k].real(), Y[k].imag())); }
        pr.push_back(a); py.push_back(b);
    }
    C.assign(nBand, 0.0); N.assign(nBand, 0.0);
    const int T = (int)pr.size(); if (T < 4) return;
    double k = 0;
    for (int b = 0; b < nBand; ++b) {
        double mr = 0, my = 0; for (int t = 0; t < T; ++t) { mr += pr[t][b]; my += py[t][b]; } mr /= T; my /= T;
        double cov = 0, vr = 0; for (int t = 0; t < T; ++t) { cov += (pr[t][b] - mr) * (py[t][b] - my); vr += (pr[t][b] - mr) * (pr[t][b] - mr); }
        const double a = vr > 0 ? std::max(0.0, cov / vr) : 0.0;
        C[b] = std::min(my, a * mr); N[b] = std::max(0.0, my - C[b]);
        const double f0 = b * bandHz; if (f0 >= coreLo && f0 < coreHi) k += C[b];
    }
    if (k > 0) for (int b = 0; b < nBand; ++b) { C[b] /= k; N[b] /= k; }
}

struct Paired { double mean = 0, se = 0; int n = 0, won = 0; double keptMin = 1.0; };
inline Paired paired(const std::vector<double>& d, const std::vector<double>& kept) {
    Paired p; p.n = (int)d.size(); if (!p.n) return p;
    for (double v : d) { p.mean += v; if (v > 0) ++p.won; }
    p.mean /= p.n;
    double v2 = 0; for (double v : d) v2 += (v - p.mean) * (v - p.mean);
    p.se = p.n > 1 ? std::sqrt(v2 / (p.n - 1) / p.n) : 0.0;
    for (double k : kept) p.keptMin = std::min(p.keptMin, k);
    return p;
}

} // namespace abscore
