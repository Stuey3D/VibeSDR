// vibe_orbcomm_tx.h — a TEST transmitter for the ORBCOMM downlink (tests only; never linked into a server's path).
// Packets → minor frames (sync, zero fill, the rest) → LSB-first bits → ±90° SDPSK steps (1 = −90°, Kenny's table) → RRC (α 0.4) at any
// rate, with a carrier offset, a Doppler slope, a timing error and white noise at a chosen Es/N0. Same published format
// as vibe_orbcomm.h (sources there).
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include "vibe_orbcomm.h"

namespace vibeorb { namespace tx {

/** A 12-byte packet of `type` with data bytes 1–9 (the check bytes are set). */
inline std::vector<uint8_t> packet(uint8_t type, std::initializer_list<uint8_t> data) {
    std::vector<uint8_t> p(12, 0); p[0] = type;
    size_t i = 1; for (uint8_t d : data) if (i < 10) p[i++] = d;
    fletcherSet(p.data(), 12);
    return p;
}
inline std::vector<uint8_t> syncPacket(int id, int channel, int frame) {
    std::vector<uint8_t> p = { 0x65, 0xA8, 0xF9, uint8_t(id), 0x10, uint8_t(channel & 0xFF), uint8_t((frame & 15) << 4 | 1), 0, 0, 0, 0, 0 };
    fletcherSet(p.data(), 12);
    return p;
}
/** An ephemeris packet for an ECEF state (m, m/s) and a GPS week / time of week (the decoder's scaling, inverted). */
inline std::vector<uint8_t> ephemerisPacket(int id, const double pos[3], const double vel[3], int week, int tow) {
    std::vector<uint8_t> p(24, 0); p[0] = kEphemeris; p[1] = uint8_t(id);
    auto put = [&](int k, double v, double max) {
        long raw = std::lround((v + max) * 1048576.0 / (2 * max)); raw = std::max(0L, std::min(1048575L, raw));
        for (int i = 0; i < 20; i++) if (raw >> i & 1) { const int b = 20 * k + i; p[size_t(2 + b / 8)] |= uint8_t(1u << (b % 8)); }
    };
    put(0, pos[0], kEphRmax); put(1, pos[1], kEphRmax); put(2, pos[2], kEphRmax);
    put(3, vel[0], kEphVmax); put(4, vel[1], kEphVmax); put(5, vel[2], kEphVmax);
    p[17] = uint8_t(tow); p[18] = uint8_t(tow >> 8); p[19] = uint8_t(tow >> 16);
    p[20] = uint8_t(week); p[21] = uint8_t(week >> 8);
    fletcherSet(p.data(), 24);
    return p;
}
/** One minor frame (600 bytes): sync, zero fill, then `body` packets, then fill to the end. */
inline std::vector<uint8_t> minorFrame(int id, int channel, int frame, const std::vector<std::vector<uint8_t>>& body) {
    std::vector<uint8_t> f;
    auto add = [&](const std::vector<uint8_t>& p) { f.insert(f.end(), p.begin(), p.end()); };
    add(syncPacket(id, channel, frame));
    add(packet(kFill, {}));
    for (auto& p : body) if (f.size() + p.size() <= 600) add(p);
    uint8_t k = 1;
    while (f.size() < 600) { add(packet(kFill, { uint8_t(0x8C ^ k), 0x69, 0xF6, 0x3A, 0x73, 0x99, 0x1A, 0xA6, k })); k++; }
    f.resize(600);
    return f;
}

struct Signal {
    double fs = 250000;        ///< output rate
    double offsetHz = 0;       ///< carrier offset at t = 0
    double slopeHzS = 0;       ///< Doppler rate
    double clockPpm = 0;       ///< symbol-clock error
    double esn0Db = 30;        ///< per-symbol SNR (noise over the output rate's bandwidth scaled to Es/N0)
    double amp = 0.3;
    unsigned seed = 1;
    double phaseWalkHz = 0;    ///< oscillator phase noise: a random walk whose rate wanders by about this (Hz rms)
};
/** Bytes → complex samples (cf32) at s.fs. */
inline std::vector<std::complex<float>> modulate(const std::vector<uint8_t>& bytes, const Signal& s) {
    std::vector<int> bits; bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes) for (int i = 0; i < 8; i++) bits.push_back(b >> i & 1);   // LSB first
    // the bit IS the step: 1 = −90°, 0 = +90° (Kenny's table — the step itself is the differential coding)
    std::vector<std::complex<double>> sym; sym.reserve(bits.size());
    double ph = 0;
    for (int I : bits) { ph += I ? -M_PI / 2 : M_PI / 2; sym.push_back(std::polar(1.0, ph)); }
    const double sps = s.fs / (kBaud * (1 + s.clockPpm * 1e-6));
    const int span = 6;
    auto rrc = [](double t, double a) {
        if (std::fabs(t) < 1e-9) return 1.0 - a + 4.0 * a / M_PI;
        if (std::fabs(std::fabs(4.0 * a * t) - 1.0) < 1e-9) return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
        return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
    };
    const size_t N = size_t(double(sym.size()) * sps) + size_t(2 * span * sps);
    std::vector<std::complex<float>> out(N);
    std::mt19937 rng(s.seed);
    std::normal_distribution<double> g(0, 1);
    // noise: Es/N0 → per-sample σ (complex) at fs: N0 = Es / snr, σ² = N0 · fs (Es = amp² · Tsym · |rrc|² ≈ amp² / baud)
    const double es = s.amp * s.amp / kBaud;
    const double n0 = es / std::pow(10.0, s.esn0Db / 10.0);
    const double sigma = std::sqrt(n0 * s.fs / 2);
    double phase = 0, walk = 0;
    for (size_t n = 0; n < N; n++) {
        const double t = double(n) / sps - span;            // in symbols
        const long k0 = long(std::floor(t)) - span, k1 = long(std::ceil(t)) + span;
        std::complex<double> v = 0;
        for (long k = std::max(0L, k0); k <= k1 && k < long(sym.size()); k++) v += sym[size_t(k)] * rrc(t - double(k), kRrcAlpha);
        if (s.phaseWalkHz > 0) walk += s.phaseWalkHz * 0.0045 * g(rng), walk *= 0.99999;   // a wander of the frequency, ≈ 0.1 s
        const double f = s.offsetHz + s.slopeHzS * double(n) / s.fs + walk;
        phase += 2 * M_PI * f / s.fs;
        v *= std::polar(s.amp, phase);
        out[n] = std::complex<float>(float(v.real() + sigma * g(rng)), float(v.imag() + sigma * g(rng)));
    }
    return out;
}

}}  // namespace vibeorb::tx
