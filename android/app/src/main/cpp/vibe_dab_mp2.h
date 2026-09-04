// vibe_dab_mp2.h — MPEG-1/2 Layer II decoder.
//
// ★★★ WHY WE DECODE THIS ONE OURSELVES. Measured in Chrome on 2026-09-04: the browser decodes
//     every DAB+ AAC variant and REFUSES real Layer II — isTypeSupported('audio/mpeg') answers
//     about MP3, then decodeAudioData throws on actual Layer II bytes. Layer II is the BBC
//     national multiplex, so "hand the bitstream to the OS" would ship with R1-6 Music silent.
//     MP2's patents expired years ago, which makes it the one codec we are free to implement.
//
// ★★ THE QUANTISATION CONSTANTS ARE DERIVED, NOT TABULATED. ISO gives C and D per level count in
//    a table; both fall out of the level count itself — C = 2^b / nlevels, and D is whatever
//    makes the MIDDLE code dequantise to exactly zero. Computing them removes ~40 magic numbers
//    and, more usefully, makes "the middle code is silence" a property the code cannot get wrong.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vibe_dab_mp2_tables.h"

namespace vibedab {

/** MSB-first bit reader over a frame. */
class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    uint32_t get(int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i) {
            const size_t byte = pos_ >> 3;
            const uint32_t bit = byte < n_ ? (d_[byte] >> (7 - (pos_ & 7))) & 1u : 0u;
            v = (v << 1) | bit;
            ++pos_;
        }
        return v;
    }
    size_t bitPos() const { return pos_; }
    bool   overrun() const { return (pos_ >> 3) > n_; }
private:
    const uint8_t* d_; size_t n_; size_t pos_ = 0;
};

/** Bits needed for one sample at `nlevels`: the smallest b with 2^b > nlevels. */
inline constexpr int quantBits(int nlevels) {
    int b = 0; while ((1 << b) <= nlevels) ++b; return b;
}
/** ISO groups three samples into one codeword when nlevels is 3, 5 or 9. */
inline constexpr bool quantGrouped(int nlevels) { return nlevels == 3 || nlevels == 5 || nlevels == 9; }
/** Codeword width: ceil(3 · log2(nlevels)) for grouped, else the per-sample width. */
inline constexpr int quantCodeBits(int nlevels) {
    if (!quantGrouped(nlevels)) return quantBits(nlevels);
    int b = 0; long long cap = 1; const long long need = (long long)nlevels * nlevels * nlevels;
    while (cap < need) { cap <<= 1; ++b; }
    return b;
}

/** Dequantise one code. ★ The MSB is inverted and the result read as a signed fraction — which is
 *  why the middle code lands exactly on zero, and why D is derivable rather than tabulated. */
inline float dequant(int q, int nlevels) {
    const int b = quantBits(nlevels);
    const int half = 1 << (b - 1);
    auto frac = [&](int code) {
        int v = code ^ half;                       // invert the MSB
        if (v >= half) v -= (half << 1);           // two's complement in b bits
        return float(v) / float(half);
    };
    const float C = float(1 << b) / float(nlevels);
    const float Doff = -frac((nlevels - 1) / 2);   // makes the middle code silent
    return C * (frac(q) + Doff);
}

/** Scalefactors: ISO's 63-entry table is exactly 2 · 2^(-i/3). */
inline float scaleFactor(int i) { return i < 63 ? float(2.0 * std::pow(2.0, -double(i) / 3.0)) : 0.0f; }

struct Mp2Info {
    int sampleRateHz = 0;
    int bitrateKbps  = 0;
    int channels     = 0;
    int frameBytes   = 0;
    bool lsf         = false;   ///< MPEG-2 low sampling frequency (24 kHz for DAB)
    int  mode        = 0;       ///< 0 stereo, 1 JOINT stereo, 2 dual channel, 3 mono
    int  modeExt     = 0;       ///< joint stereo: (modeExt+1)*4 = the bound sub-band
    bool valid       = false;
};

/** Parse a frame header. Returns validity; does not consume anything. */
inline Mp2Info mp2Header(const uint8_t* d, size_t n) {
    Mp2Info f;
    if (n < 4) return f;
    if (d[0] != 0xFF || (d[1] & 0xE0) != 0xE0) return f;
    const int ver   = (d[1] >> 3) & 3;             // 3 = MPEG1, 2 = MPEG2
    const int layer = (d[1] >> 1) & 3;             // 2 = Layer II
    if (layer != 2 || (ver != 3 && ver != 2)) return f;
    static const int kBr1[15] = { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384 };
    static const int kBr2[15] = { 0, 8,16,24,32,40,48,56,64,80,96,112,128,144,160 };
    static const int kSr1[3]  = { 44100, 48000, 32000 };
    const int bi = (d[2] >> 4) & 15, si = (d[2] >> 2) & 3;
    if (bi == 0 || bi == 15 || si == 3) return f;
    f.lsf          = (ver == 2);
    f.bitrateKbps  = f.lsf ? kBr2[bi] : kBr1[bi];
    f.sampleRateHz = f.lsf ? kSr1[si] / 2 : kSr1[si];
    f.mode         = (d[3] >> 6) & 3;
    f.modeExt      = (d[3] >> 4) & 3;
    f.channels     = f.mode == 3 ? 1 : 2;
    const int pad  = (d[2] >> 1) & 1;
    f.frameBytes   = 144 * f.bitrateKbps * 1000 / f.sampleRateHz + pad;
    f.valid        = true;
    return f;
}

/** Which allocation table applies (TS 103 466 clause 5.1). */
inline const AllocTable& allocTableFor(const Mp2Info& f) {
    if (f.lsf) return kAllocTable24;
    const int perCh = f.bitrateKbps / (f.channels == 1 ? 1 : 2);
    return (perCh <= 48) ? kAllocTable48Narrow : kAllocTable48Wide;
}

/** Layer II decoder: one frame in, 1152 samples per channel out. */
class Mp2Decoder {
public:
    /** @return samples per channel written, or 0 on a bad frame. */
    int decode(const uint8_t* frame, size_t n, std::vector<float>& out) {
        const Mp2Info f = mp2Header(frame, n);
        if (!f.valid || size_t(f.frameBytes) > n) return 0;
        info_ = f;
        const AllocTable& tab = allocTableFor(f);
        const int nch = f.channels, sblimit = tab.sblimit;

        BitReader br(frame, size_t(f.frameBytes));
        br.get(32);                                   // header
        if (((frame[1] >> 0) & 1) == 0) br.get(16);   // protection bit CLEAR = CRC present

        /* ★★★ JOINT STEREO — AND THE REAL BBC USES IT.
         *
         *  In mode 1 the sub-bands from `bound` upwards are coded ONCE and shared by both
         *  channels (intensity stereo): one allocation, one set of samples, but a scalefactor
         *  each. Reading two allocations up there desynchronises the bit reader and everything
         *  after it is noise.
         *
         *  ★★ THIS IS EXACTLY WHY VERIFYING AGAINST A FILE I ENCODED MYSELF WAS NOT ENOUGH. My
         *     ffmpeg reference was plain stereo (mode 0), so the decoder matched to 3.7e-07 and
         *     looked finished. The first real BBC frame is JOINT stereo, and the same decoder
         *     produced pure noise. A test built from convenient input agrees with the bug.
         *  ★ bound = (modeExt + 1) x 4, capped at sblimit.
         */
        const int bound = (f.mode == 1) ? std::min(sblimit, (f.modeExt + 1) * 4) : sblimit;

        int alloc[2][32] = {};
        for (int sb = 0; sb < sblimit; ++sb) {
            if (!tab.sb[sb].nbal) continue;
            if (sb < bound) {
                for (int ch = 0; ch < nch; ++ch)
                    alloc[ch][sb] = tab.sb[sb].nlevels[br.get(tab.sb[sb].nbal)];
            } else {
                const int a2 = tab.sb[sb].nlevels[br.get(tab.sb[sb].nbal)];
                for (int ch = 0; ch < nch; ++ch) alloc[ch][sb] = a2;   // shared
            }
        }

        int scfsi[2][32] = {};
        for (int sb = 0; sb < sblimit; ++sb)
            for (int ch = 0; ch < nch; ++ch)
                if (alloc[ch][sb]) scfsi[ch][sb] = int(br.get(2));

        float sf[2][32][3] = {};
        for (int sb = 0; sb < sblimit; ++sb)
            for (int ch = 0; ch < nch; ++ch) {
                if (!alloc[ch][sb]) continue;
                // ★ scfsi says which of the three 8-sample parts share a scalefactor. Reading the
                //   wrong count here desynchronises the whole rest of the frame, which is why a
                //   Layer II bug is usually total rather than subtle.
                switch (scfsi[ch][sb]) {
                    case 0: for (int g = 0; g < 3; ++g) sf[ch][sb][g] = scaleFactor(int(br.get(6))); break;
                    case 1: { float a = scaleFactor(int(br.get(6))); float b = scaleFactor(int(br.get(6)));
                              sf[ch][sb][0] = sf[ch][sb][1] = a; sf[ch][sb][2] = b; break; }
                    case 2: { float a = scaleFactor(int(br.get(6)));
                              sf[ch][sb][0] = sf[ch][sb][1] = sf[ch][sb][2] = a; break; }
                    default: { float a = scaleFactor(int(br.get(6))); float b = scaleFactor(int(br.get(6)));
                               sf[ch][sb][0] = a; sf[ch][sb][1] = sf[ch][sb][2] = b; break; }
                }
            }

        out.assign(size_t(1152) * size_t(nch), 0.0f);
        for (int gr = 0; gr < 12; ++gr) {
            float sample[2][3][32] = {};
            for (int sb = 0; sb < sblimit; ++sb)
                for (int ch = 0; ch < nch; ++ch) {
                    const int nl = alloc[ch][sb];
                    if (!nl) continue;
                    // ★ Above the bound the samples are read ONCE and shared; each channel then
                    //   applies its OWN scalefactor, which is what makes it intensity stereo.
                    if (sb >= bound && ch > 0) continue;
                    float raw[3];
                    if (quantGrouped(nl)) {
                        uint32_t code = br.get(quantCodeBits(nl));
                        for (int k = 0; k < 3; ++k) { raw[k] = dequant(int(code % uint32_t(nl)), nl); code /= uint32_t(nl); }
                    } else {
                        const int bits = quantCodeBits(nl);
                        for (int k = 0; k < 3; ++k) raw[k] = dequant(int(br.get(bits)), nl);
                    }
                    if (sb >= bound)
                        for (int c2 = 0; c2 < nch; ++c2)
                            for (int k = 0; k < 3; ++k) sample[c2][k][sb] = raw[k] * sf[c2][sb][gr >> 2];
                    else
                        for (int k = 0; k < 3; ++k) sample[ch][k][sb] = raw[k] * sf[ch][sb][gr >> 2];
                }
            for (int k = 0; k < 3; ++k)
                for (int ch = 0; ch < nch; ++ch)
                    synth(ch, sample[ch][k], &out[(size_t(gr) * 3 + size_t(k)) * 32 * size_t(nch) + size_t(ch)], nch);
        }
        return 1152;
    }

    const Mp2Info& info() const { return info_; }
    void reset() { std::memset(v_, 0, sizeof v_); }

private:
    /** ISO synthesis: shift, matrix, build U, window, fold by 16. */
    void synth(int ch, const float* s, float* out, int stride) {
        float* V = v_[ch];
        std::memmove(V + 64, V, sizeof(float) * (1024 - 64));
        for (int i = 0; i < 64; ++i) {
            float a = 0;
            for (int k = 0; k < 32; ++k) a += cosTab(i, k) * s[k];
            V[i] = a;
        }
        float U[512];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 32; ++j) {
                U[i * 64 + j]      = V[i * 128 + j];
                U[i * 64 + 32 + j] = V[i * 128 + 96 + j];
            }
        for (int j = 0; j < 32; ++j) {
            float a = 0;
            for (int i = 0; i < 16; ++i) a += U[j + 32 * i] * kSynthWindow[j + 32 * i];
            out[size_t(j) * size_t(stride)] = a;
        }
    }
    /** N[i][k] = cos((16+i)(2k+1)π/64), built once. */
    static float cosTab(int i, int k) {
        static float t[64][32];
        static bool init = false;
        if (!init) {
            for (int a = 0; a < 64; ++a)
                for (int b = 0; b < 32; ++b)
                    t[a][b] = float(std::cos((16 + a) * (2 * b + 1) * M_PI / 64.0));
            init = true;
        }
        return t[i][k];
    }

    float v_[2][1024] = {};
    Mp2Info info_;
};

}  // namespace vibedab
