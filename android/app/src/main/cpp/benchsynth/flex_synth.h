// tools/paging-bench/flex_synth.h — a FLEX TRANSMITTER of our own, for the tests and the bench only (V12, 2026-10-31).
// Built from the same published layouts as the receiver (decoders/paging_decoder.cpp; sources: docs/v12/DECODERS-WAVE2.md
// section 17), so it proves the receiver reads what the layouts say — and a PUBLIC recording (not in git) proves the
// layouts are the ones on air. Every page it makes is synthetic.
#pragma once
#include "../decoders/paging_decoder.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace flexsyn {

using vibe::PagingDecoder;

struct TxPage {
    uint32_t cap;               ///< capcode
    bool     lng = false;       ///< a long (two-word) address
    std::string kind;           ///< "alpha", "numeric", "tone"
    std::string text;
    int      phase = 0;         ///< 0..3 = A..D (must exist at the frame's speed)
    int      msgNum = 1;
    int      frag = 3;          ///< 3 = a first fragment, then 0, 1, 2 …
    bool     cont = false;      ///< continued in a later frame
};
struct Sym { float level; double sec; };   ///< ±1 outer, ±1/3 inner (the discriminator in units of 4.8 kHz)

inline uint32_t cwOf(uint32_t v) { return PagingDecoder::encodeWord(PagingDecoder::flexInfo(v)); }

/** One phase's 88 words (VALUES, i0 = LSB) for its pages. */
inline std::vector<uint32_t> phaseWords(const std::vector<TxPage>& pages) {
    std::vector<uint32_t> w(88, 0x1FFFFFu);   // idle
    int nAddr = 0; for (auto& p : pages) nAddr += p.lng ? 2 : 1;
    const int aStart = 1, vStart = aStart + nAddr;
    w[0] = PagingDecoder::flexWithChecksum((uint32_t)vStart << 10);   // BIW1: no extra BIWs, the vector start
    int a = aStart, msg = vStart + nAddr;   // messages after the vectors
    for (const TxPage& p : pages) {
        const int j = vStart + (a - aStart);
        if (!p.lng) w[(size_t)a] = p.cap + 0x8000u;
        else { const uint32_t x = p.cap - 2068480u - 1u; w[(size_t)a] = x % 32768u + 1u; w[(size_t)a + 1] = 2097151u - x / 32768u; }
        std::vector<uint32_t> m;   // the message words (values)
        uint32_t vec = 0;
        if (p.kind == "alpha") {
            std::vector<uint32_t> chars;
            if (p.frag == 3) chars.push_back(0x55);   // the signature slot
            for (unsigned char c : p.text) chars.push_back(c & 0x7Fu);
            while (chars.size() % 3) chars.push_back(3);   // ETX fill
            m.push_back(((uint32_t)p.msgNum << 13) | ((uint32_t)p.frag << 11) | ((p.cont ? 1u : 0u) << 10));
            for (size_t k = 0; k < chars.size(); k += 3) m.push_back(chars[k] | (chars[k + 1] << 7) | (chars[k + 2] << 14));
            m[0] |= PagingDecoder::flexAlphaCheck(m);
            vec = (5u << 4) | ((uint32_t)m.size() << 14);
        } else if (p.kind == "numeric") {
            std::vector<int> bits = {0, 0};   // K4, K5
            static const char* kN = "0123456789*U -][";
            for (char c : p.text) { int d = 12; for (int q = 0; q < 16; ++q) if (kN[q] == c) d = q; for (int b = 0; b < 4; ++b) bits.push_back((d >> b) & 1); }
            while (bits.size() % 21) { const int pos = (int)((bits.size() - 2) % 4); bits.push_back(pos == 2 || pos == 3 ? 1 : 0); }   // fill with C (1100)
            for (size_t k = 0; k < bits.size(); k += 21) { uint32_t x = 0; for (int b = 0; b < 21; ++b) x |= (uint32_t)bits[k + (size_t)b] << b; m.push_back(x); }
            vec = (3u << 4) | ((uint32_t)(m.size() - 1) << 14);
        } else {   // tone only: a short message vector, t = 01
            vec = (2u << 4) | (1u << 7);
        }
        if (p.kind != "tone") {
            // a long address's first message word rides in its second vector word
            size_t from = 0;
            if (p.lng) { w[(size_t)j + 1] = m[0]; from = 1; }
            vec |= (uint32_t)msg << 7;
            for (size_t k = from; k < m.size(); ++k) w[(size_t)msg++] = m[k];
        } else if (p.lng) w[(size_t)j + 1] = 0;
        w[(size_t)j] = PagingDecoder::flexWithChecksum(vec);
        a += p.lng ? 2 : 1;
    }
    return w;
}

/** One frame's symbols. bps/levels as the A word names them; the data sense opposite to the sync constants'. */
inline std::vector<Sym> frame(int bps, int levels, int cycle, int frameNo, const std::vector<TxPage>& pages) {
    std::vector<Sym> out;
    const double t16 = 1.0 / 1600;
    auto bits2 = [&](uint64_t v, int n) { for (int k = n - 1; k >= 0; --k) out.push_back({((v >> k) & 1) ? 1.f : -1.f, t16}); };
    const uint16_t code = PagingDecoder::flexModeCode(bps, levels);
    bits2(0xAAAAAAAAull, 32);                                            // comma
    bits2(((uint64_t)code << 16) | 0xA6C6u, 32); bits2(0xAAAAu, 16);      // A, B
    bits2(~(((uint64_t)code << 16) | 0xA6C6u) & 0xFFFFFFFFull, 32);      // A inverted
    bits2(~(uint64_t)cwOf(PagingDecoder::flexWithChecksum(((uint32_t)cycle << 4) | ((uint32_t)frameNo << 8))) & 0xFFFFFFFFull, 32);   // FIW (data sense)
    const int baud = (bps == 1600 || (bps == 3200 && levels == 4)) ? 1600 : 3200;
    const double ts = 1.0 / baud;
    for (int k = 0; k < baud / 40; ++k) out.push_back({(k & 1) ? -1.f : 1.f, ts});   // Sync 2 (a comma)
    std::vector<std::vector<uint32_t>> ph(4);
    for (int p = 0; p < 4; ++p) { std::vector<TxPage> mine; for (auto& x : pages) if (x.phase == p) mine.push_back(x); ph[(size_t)p] = phaseWords(mine); }
    for (int bl = 0; bl < 11; ++bl) {
        std::vector<std::vector<int>> pb(4, std::vector<int>(256));
        for (int p = 0; p < 4; ++p)
            for (int c = 0; c < 8; ++c) {
                const uint32_t cw = cwOf(ph[(size_t)p][(size_t)(bl * 8 + c)]);
                for (int k = 0; k < 32; ++k) pb[(size_t)p][(size_t)(8 * k + c)] = (int)((cw >> (31 - k)) & 1u);
            }
        auto lvl = [&](int msb, int innerBit) { const float a = innerBit ? (1.f / 3.f) : 1.f; return msb ? -a : a; };   // data sense: complemented
        if (levels == 2 && baud == 1600) for (int k = 0; k < 256; ++k) out.push_back({lvl(pb[0][(size_t)k], 0), ts});
        else if (levels == 2) for (int k = 0; k < 256; ++k) { out.push_back({lvl(pb[0][(size_t)k], 0), ts}); out.push_back({lvl(pb[2][(size_t)k], 0), ts}); }
        else if (baud == 1600) for (int k = 0; k < 256; ++k) out.push_back({lvl(pb[0][(size_t)k], pb[2][(size_t)k]), ts});
        else for (int k = 0; k < 256; ++k) { out.push_back({lvl(pb[0][(size_t)k], pb[1][(size_t)k]), ts}); out.push_back({lvl(pb[2][(size_t)k], pb[3][(size_t)k]), ts}); }
    }
    return out;
}

/** The symbols as FM IQ (±4.8 kHz at level 1), a one-pole shaping at 0.6 × the symbol rate, noise for `ebn0Db` at the
 *  1600 bit rate's Eb (∞ = none), `clockErr`, `invert` (every level negated), then the receiver's channel filter. */
inline std::vector<std::complex<float>> fmIq(const std::vector<Sym>& s, double fs, double clockErr, bool invert, double snrDb,
                                             uint32_t seed, double passHz, double lead = 0.3, double tail = 0.3) {
    std::mt19937 rng(seed); std::normal_distribution<float> g(0.f, 1.f);
    double total = lead + tail; for (auto& x : s) total += x.sec / (1.0 + clockErr);
    const int n = (int)(total * fs);
    // ★ snrDb = carrier power over the noise in the channel filter's passband (CNR)
    const double sigma = std::isfinite(snrDb) ? std::sqrt(0.5 * (fs / passHz) / std::pow(10.0, snrDb / 10.0)) : 0.0;
    std::vector<std::complex<float>> x((size_t)n);
    double y = 0, ph = 0.4, t0 = lead; size_t k = 0;
    for (int i = 0; i < n; ++i) {
        const double t = i / fs;
        while (k < s.size() && t >= t0 + s[k].sec / (1.0 + clockErr)) { t0 += s[k].sec / (1.0 + clockErr); ++k; }
        double lv = 0, rate = 1600;
        if (t >= lead && k < s.size()) { lv = invert ? -s[k].level : s[k].level; rate = 1.0 / s[k].sec; }
        y += (1.0 - std::exp(-2 * M_PI * 0.6 * rate / fs)) * (lv - y);
        ph += 2 * M_PI * 4800.0 * y / fs;
        x[(size_t)i] = std::complex<float>((float)(std::cos(ph) + sigma * g(rng)), (float)(std::sin(ph) + sigma * g(rng)));
    }
    const int T = 63; std::vector<float> h(T); double hs = 0;
    for (int q = 0; q < T; ++q) {
        const double m = q - (T - 1) / 2.0, fc = 0.5 * passHz / fs;
        const double v = (m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m)) * (0.54 - 0.46 * std::cos(2 * M_PI * q / (T - 1)));
        h[(size_t)q] = (float)v; hs += v;
    }
    for (auto& v : h) v = (float)(v / hs);
    std::vector<std::complex<float>> o((size_t)n);
    for (int i = 0; i < n; ++i) { std::complex<float> acc(0, 0); for (int q = 0; q < T; ++q) { const int j = i - q; if (j >= 0) acc += h[(size_t)q] * x[(size_t)j]; } o[(size_t)i] = acc; }
    return o;
}

} // namespace flexsyn
