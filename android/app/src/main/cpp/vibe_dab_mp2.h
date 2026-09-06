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
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
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

/** ★★★ THE LAYER II CRC, WHICH WE WERE READING PAST AND THROWING AWAY.
 *
 *  MPEG-1 Layer II protects the second half of the header, the bit ALLOCATION and the scfsi with
 *  a CRC-16 (x^16 + x^15 + x^2 + 1, preset all ones). Those are exactly the fields that decide how
 *  every following bit is interpreted, which is why the standard protects them and nothing else.
 *
 *  ★★★ MEASURED, from Stuart's own 41-second recording off the live receiver: normal audio peaks
 *  at a very consistent 19,600 of 32,767 — the flatness of broadcast limiting — and three times
 *  in that recording, at 6.24 s, 18.44 s and 31.56 s, the level jumps 2.6x and slams into full
 *  scale for 140-160 ms. Spacing 12-13 seconds. That is his "clean for 10-15 seconds with a
 *  1 second burst", and it is not programme material: a limited broadcast does not do that.
 *
 *  It is a corrupt frame decoded as though it were sound. A wrong allocation desynchronises the
 *  bit reader and every sample after it is noise — loud noise, because the quantiser scales it.
 *  Every real receiver checks this CRC and conceals the frame; we skipped the field with
 *  `br.get(16)` and decoded the wreckage. That is why a V4 and aerial with a year of flawless
 *  DAB under OWRX broke up on ours: not weaker demodulation — no error checking at the end of it.
 *
 *  ★ 0.05% of samples were at full scale. A tiny number, and all of it audible.
 */
inline uint16_t mp2Crc16(const uint8_t* d, size_t bitStart, size_t bitEnd) {
    uint16_t crc = 0xFFFF;
    const auto push = [&crc](unsigned bit) {
        const bool msb = (crc & 0x8000u) != 0;
        crc = uint16_t(crc << 1);
        if (msb != (bit != 0)) crc ^= 0x8005u;
    };
    // The header's last two bytes are protected, the first two (sync and rates) are not.
    for (size_t i = 16; i < 32; ++i) push((d[i >> 3] >> (7 - (i & 7))) & 1u);
    for (size_t i = bitStart; i < bitEnd; ++i) push((d[i >> 3] >> (7 - (i & 7))) & 1u);
    return crc;
}

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
        ++sinceNoSync_;
        const Mp2Info f = mp2Header(frame, n);
        /* ★★★ SEPARATE THE TWO WAYS A FRAME DIES, because they have opposite causes and only one
         *  of them is fixable by us. A bad HEADER means the bytes we were handed are not an MP2
         *  frame at all — wrong length, wrong sync — which is an assembly or framing fault. A
         *  failed CRC means the bytes ARE a frame and the bits inside it are wrong, which is
         *  residual bit error rate out of the Viterbi. Counted apart, "20.6% of frames bad" stops
         *  being one number and becomes a diagnosis. */
        if (!f.valid || size_t(f.frameBytes) > n) {
            ++hdrBad_;
            /* ★★★ AND WHICH KIND OF "not a frame" IT IS, because hdrBad alone cannot tell a
             *  FRAMING fault from a BIT ERROR and I read it as if it could. An MPEG frame starts
             *  with an 11-bit sync word of all ones. If those bits are present, we are looking at
             *  a real frame header whose other fields are damaged — that is residual bit error
             *  rate. If they are absent, these bytes are not the start of a frame at all, and the
             *  fault is in how the logical frame was assembled or where it was cut.
             *  ★ noSync counts the second kind: the one that is ours to fix. */
            const bool sync = n >= 2 && frame[0] == 0xFF && (frame[1] & 0xE0) == 0xE0;
            if (!sync) {
                ++hdrNoSync_;
                /* ★★★ HOW FAR APART ARE THEY? A steady 3% loss can be periodic — a sub-block or
                 *  CIF boundary landing wrong — or scattered, which would mean something else
                 *  entirely. The two need completely different fixes and the rate alone cannot
                 *  tell them apart, so the GAP between consecutive failures is recorded: a ring of
                 *  the last eight, published as text. All the same number is a structural fault
                 *  with a period; a spread of numbers is not. */
                if (gapRing_.size() >= 8) gapRing_.erase(gapRing_.begin());
                gapRing_.push_back(sinceNoSync_);
                sinceNoSync_ = 0;
            }
            else if (f.valid) ++hdrTooLong_;     // real header, but longer than the bytes in hand
            return 0;
        }
        info_ = f;
        const AllocTable& tab = allocTableFor(f);
        const int nch = f.channels, sblimit = tab.sblimit;

        BitReader br(frame, size_t(f.frameBytes));
        br.get(32);                                   // header
        // ★ Protection bit CLEAR = a CRC is present. Keep it: it is checked below, once the
        //   fields it covers have been read. See mp2Crc16.
        const bool haveCrc = ((frame[1] >> 0) & 1) == 0;
        lastHadCrc_ = haveCrc;
        const uint16_t wantCrc = haveCrc ? uint16_t(br.get(16)) : 0;
        const size_t crcFrom = br.bitPos();

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

        /* ★★★ CHECK IT HERE, where the protected fields end. A frame that fails is REFUSED
         *  rather than decoded: the allocation is what tells the reader how to interpret every
         *  following bit, so a wrong one does not degrade the audio, it replaces it with noise at
         *  full scale. Refusing costs 24 ms of silence; decoding it costs the listener's ears. */
        if (haveCrc && mp2Crc16(frame, crcFrom, br.bitPos()) != wantCrc) { ++crcBad_; return 0; }

        scfIdx_.clear();
        float sf[2][32][3] = {};
        /* ★ Raw 6-bit indices kept per (channel, sub-band, granule) so the squeal guard below can
         *  judge them against their own context BEFORE they become gains. -1 = not transmitted. */
        int sfIdx[2][32][3];
        for (int c = 0; c < 2; ++c) for (int b = 0; b < 32; ++b)
            for (int g = 0; g < 3; ++g) sfIdx[c][b][g] = -1;
        for (int sb = 0; sb < sblimit; ++sb)
            for (int ch = 0; ch < nch; ++ch) {
                if (!alloc[ch][sb]) continue;
                // ★ scfsi says which of the three 8-sample parts share a scalefactor. Reading the
                //   wrong count here desynchronises the whole rest of the frame, which is why a
                //   Layer II bug is usually total rather than subtle.
                /* ★ Keep the raw 6-bit INDICES as well as the gains. DAB protects the three
                 *  most significant bits of each transmitted scale factor with a CRC-8 (TS 103
                 *  466), and a scale factor is a logarithmic gain — one wrong value makes a
                 *  sub-band ring, which is the tonal SQUEAL Stuart hears rather than the bubbling
                 *  mud of ordinary MP2 bit errors. Collected in bitstream order, per sub-band. */
                auto take6 = [&](int g0, int g1) {
                    const int idx = int(br.get(6));
                    scfIdx_.push_back({ sb, uint8_t(idx) });
                    for (int g = g0; g <= g1; ++g) sfIdx[ch][sb][g] = idx;
                };
                switch (scfsi[ch][sb]) {
                    case 0:  take6(0, 0); take6(1, 1); take6(2, 2); break;
                    case 1:  take6(0, 1); take6(2, 2);              break;
                    case 2:  take6(0, 2);                           break;
                    default: take6(0, 0); take6(1, 2);              break;
                }
            }

        /* ★★★ THE SQUEAL GUARD — CATCH IT AS A GAIN, NOT AS A WAVEFORM.
         *
         *  ★★★ WHAT THE SQUEAL ACTUALLY IS, MEASURED. The MPEG CRC covers the header, the bit
         *      allocation and the scfsi — it does NOT cover the scale factors (see mp2Crc16). So a
         *      frame whose scale factors were corrupted passes every check we make and decodes
         *      into a loud tone. On 400 captured frames the corruption was scale factors jumping
         *      more than 24 dB, on every 6th frame, in BOTH channels at once — periodic and
         *      bilateral, i.e. structural (the time deinterleaver / UEP sub-blocks), not noise.
         *
         *  ★★★ WHY HERE AND NOT ON THE OUTPUT. DabService already conceals on the decoded
         *      waveform (peak > 1.5, RMS > 6x the running mean). That fires on only about a fifth
         *      of the frames it should: measured on 10C, 1806 bad frames against 362 concealed.
         *      By then the damage is spread across 1152 samples of synthesis filter and the only
         *      remaining option is to mute the lot. A scale factor is a single logarithmic gain —
         *      here it can be REPAIRED, and only the sub-band that was hit loses anything.
         *
         *  ★★★ IT ONLY EVER MAKES A BAND QUIETER, AND THAT IS THE WHOLE SAFETY ARGUMENT.
         *      gain = 2 * 2^(-idx/3), so a SMALLER index is a LOUDER band and each index step is
         *      2.007 dB. A squeal is therefore an index far BELOW its context. Corruption in the
         *      other direction makes a band too quiet, which is inaudible against the rest of the
         *      frame and is left strictly alone. So the guard cannot dull, colour or gate clean
         *      audio: on a multiplex that is not corrupting scale factors it never fires at all.
         *      ★ That matters because 10C is the outlier — Stuart: "10c is just one multiplex the
         *        others BBC/D1/SDL we mostly fine". A fix that costs anything on 12B is not a fix.
         *
         *  ★★ CONTEXT IS THE PREVIOUS FRAME FIRST. The corruption hits both channels and all three
         *     granules of a sub-band together, so the median WITHIN the frame is corrupt too and
         *     cannot be the reference. The last frame in which this sub-band looked sane can.
         *     Falls back to the in-frame median only when there is no history — after a run of
         *     refused frames, when history is stale by construction.
         *
         *  ★ Threshold in INDICES, and deliberately at the measured pathology rather than a guess:
         *    12 indices = 24.1 dB. An erasure threshold I guessed once before turned out 13x off
         *    what measurement said, which is why this one is both env-overridable and counted.
         *    VIBE_DAB_SCF_MAX_JUMP, in indices; 0 disables the guard entirely. */
        {
            /* ★★★ OFF BY DEFAULT — IT DAMAGED CLEAN AUDIO AND I SHIPPED IT ON A GUESS.
             *  ★★★ MEASURED ON AIR, 11A, a multiplex with FIB 1.000 and 0.80% bad frames — i.e.
             *      audio with essentially nothing wrong with it: 1412 scale factors clamped in
             *      38 seconds, about 1% of every scale factor transmitted. Each one is a gain
             *      moved by up to 24 dB, and Stuart heard exactly that: "MP2 bursts of noise".
             *  ★★★ AND THE UNIT TEST SAID IT WAS INERT. It decodes FOUR frames of clean BBC
             *      Radio 4 and asserts zero clamps, and it passes. Four frames is not a sample of
             *      programme material — it never reaches a transient, and the cross-frame history
             *      the guard leans on barely exists that early. A test that agrees with the bug is
             *      worse than no test, and this file already carries that warning about the
             *      SYNTHETIC case; I wrote another one.
             *  ★★★ THE MODEL IS WRONG, NOT JUST THE NUMBER. Context is taken as the LOUDEST index
             *      the sub-band recently held, and real music moves a sub-band tens of dB between
             *      frames at every onset. Distinguishing that from corruption needs the measured
             *      distribution of legitimate frame-to-frame jumps, which we have never taken —
             *      dab-offline replays captured frames deterministically and is where it must be
             *      derived, not on air a build at a time.
             *  ★ 12 was chosen because the corruption measured >24 dB. That says where corruption
             *    IS, not where clean audio ISN'T, and only the second one makes a threshold safe.
             *  ★ The code stays, disabled, so the measurement can drive it: set
             *    VIBE_DAB_SCF_MAX_JUMP to a jump in indices (2.007 dB each) to enable it.
             *  ★ Squeal concealment is NOT gone — DabService still conceals on the decoded
             *    waveform, which is the behaviour that was on air before today. */
            static const int kMaxJump = std::getenv("VIBE_DAB_SCF_MAX_JUMP")
                                      ? atoi(std::getenv("VIBE_DAB_SCF_MAX_JUMP")) : 0;
            if (kMaxJump > 0) {
                for (int sb = 0; sb < sblimit; ++sb)
                    for (int ch = 0; ch < nch; ++ch) {
                        if (!alloc[ch][sb]) continue;
                        // In-frame median of the granules actually transmitted, as the fallback.
                        int v[3], n = 0;
                        for (int g = 0; g < 3; ++g) if (sfIdx[ch][sb][g] >= 0) v[n++] = sfIdx[ch][sb][g];
                        if (n == 0) continue;
                        for (int a = 0; a < n; ++a) for (int b = a + 1; b < n; ++b)
                            if (v[b] < v[a]) { const int t = v[a]; v[a] = v[b]; v[b] = t; }
                        const int med = v[n / 2];
                        const bool haveHist = sfHistValid_[ch][sb];
                        const int  ctx = haveHist ? int(sfHist_[ch][sb]) : med;
                        for (int g = 0; g < 3; ++g) {
                            const int idx = sfIdx[ch][sb][g];
                            if (idx < 0) continue;
                            // Smaller index = louder. Only an implausibly LOUD one is touched.
                            if (idx < ctx - kMaxJump) {
                                sfIdx[ch][sb][g] = ctx - kMaxJump;
                                ++sfClamped_;
                            }
                        }
                        /* ★ History follows the REPAIRED value, so a genuine slow crescendo still
                         *  moves the reference frame by frame and is never fought; but a single
                         *  corrupt frame cannot drag the reference loud and let the next one
                         *  through. */
                        int lo = 63;
                        for (int g = 0; g < 3; ++g) if (sfIdx[ch][sb][g] >= 0 && sfIdx[ch][sb][g] < lo)
                            lo = sfIdx[ch][sb][g];
                        sfHist_[ch][sb] = uint8_t(lo);
                        sfHistValid_[ch][sb] = true;
                    }
            }
        }
        // ★ Indices settled (and repaired) — only now turn them into gains.
        for (int sb = 0; sb < sblimit; ++sb)
            for (int ch = 0; ch < nch; ++ch)
                for (int g = 0; g < 3; ++g)
                    if (sfIdx[ch][sb][g] >= 0) sf[ch][sb][g] = scaleFactor(sfIdx[ch][sb][g]);

        // ★ Now that the scale factors are known, check DAB's own CRC over them — counting
        //   only, until the convention is measured. See tallyScfCrc.
        tallyScfCrc(frame, f.frameBytes, f.bitrateKbps);

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

    /* ★★★ THE SCALE FACTOR CRC (TS 103 466). This is the check DAB adds on top of MPEG's, and
     *  the one that matters: the MPEG CRC covers the header, the bit allocation and scfsi — all
     *  of which measured CLEAN here (2052 of 2052 frames) — and NOT the scale factors. A scale
     *  factor is a logarithmic gain, so a single corrupted one makes a sub-band ring: the tonal
     *  squeal Stuart describes, as against the "bubbling mud" of genuine MP2 sample errors, and
     *  the 2.6x level jumps clipping to full scale that his recording showed.
     *
     *  Layout, from the END of the frame: [ ... audio ... ][ X-PAD ][ ScF-CRC ][ F-PAD (2) ].
     *  Four CRC-8s at 48 kHz and >= 56 kbit/s (sub-bands 0-3, 4-7, 8-15, 16-26), two below that
     *  (0-3, 4-7). Generator G2(X) = X^8 + X^4 + X^3 + X^2 + 1 = 0x1D, over the three most
     *  significant bits of each transmitted scale factor.
     *
     *  ★★★ THE PRESET AND FINAL INVERSION ARE NOT MEASURED YET, so all four conventions are
     *  computed and COUNTED rather than acted on. Guessing wrong here fails every frame and
     *  silences the radio, and this session has already shipped two regressions from acting on
     *  an untested assumption. The variant that matches ~99% of frames on air is the right one,
     *  and it will be hard-coded with that evidence beside it. */
    struct ScfCrcTally { uint32_t checked = 0, ok[4] = {0, 0, 0, 0}; };
    const ScfCrcTally& scfCrc() const { return scfTally_; }
    /** ★ Did the last frame carry an MPEG header CRC at all? DAB may use the SCALE FACTOR CRC of
     *  TS 103 466 instead, in which case the MPEG protection bit is set and there is nothing to
     *  check here — which would make a zero refusal count meaningless. Measure before believing. */
    bool lastHadCrc() const { return lastHadCrc_; }
    /** How many scale factors the squeal guard has pulled back — published, so the underlying
     *  error rate stays visible rather than being hidden by the concealment. */
    uint32_t scfClamped() const { return sfClamped_; }
    /** ★ Frames refused for not being frames at all (framing fault) vs frames whose bits failed
     *  the CRC (bit errors). See the note at the header check. */
    uint32_t hdrBad() const { return hdrBad_; }
    uint32_t crcBad() const { return crcBad_; }
    /** ★ Of the hdrBad frames: how many carried NO sync word (a framing fault — ours) and how many
     *  carried a valid header claiming more bytes than arrived (the LSF pairing's other arm). */
    uint32_t hdrNoSync()  const { return hdrNoSync_; }
    uint32_t hdrTooLong() const { return hdrTooLong_; }
    /** ★ Frames between the last eight sync-word failures — the pattern, not just the rate. */
    std::string noSyncGaps() const {
        std::string r;
        for (size_t i = 0; i < gapRing_.size(); ++i) { if (i) r += ' '; r += std::to_string(gapRing_[i]); }
        return r;
    }
    /** ★★★ CALL THIS WHENEVER THE FRAME RUN BREAKS. The guard's reference is "what this sub-band
     *  was doing a moment ago", and after a gap it was doing it a moment ago in a different piece
     *  of music. Stale history would fight the first frames back in. */
    void resetScfHistory() {
        for (int c = 0; c < 2; ++c) for (int b = 0; b < 32; ++b) sfHistValid_[c][b] = false;
    }
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
    bool    lastHadCrc_ = false;
    struct ScfEntry { int sb; uint8_t idx; };
    std::vector<ScfEntry> scfIdx_;
    ScfCrcTally scfTally_;
    uint8_t prevScf_[4] = {0,0,0,0};
    int     prevScfN_ = 0;
    bool    prevScfValid_ = false;
    /** ★ Per (channel, sub-band) reference for the squeal guard: the loudest index this sub-band
     *  last held, after repair. Invalidated by resetScfHistory() whenever the frame run breaks. */
    uint8_t sfHist_[2][32] = {};
    bool    sfHistValid_[2][32] = {};
    uint32_t sfClamped_ = 0;
    uint32_t hdrBad_ = 0, crcBad_ = 0, hdrNoSync_ = 0, hdrTooLong_ = 0;
    uint32_t sinceNoSync_ = 0;
    std::vector<uint32_t> gapRing_;

    /** CRC-8, G2 = 0x1D, over the three MSBs of each scale factor in [sbLo, sbHi]. */
    uint8_t scfCrc8(int sbLo, int sbHi, uint8_t init, bool invert) const {
        uint8_t c = init;
        for (const ScfEntry& e : scfIdx_) {
            if (e.sb < sbLo || e.sb > sbHi) continue;
            for (int b = 2; b >= 0; --b) {                 // the three most significant bits
                const bool bit = ((e.idx >> (5 - (2 - b))) & 1) != 0;
                const bool msb = (c & 0x80) != 0;
                c = uint8_t(c << 1);
                if (msb != bit) c ^= 0x1D;
            }
        }
        return invert ? uint8_t(~c) : c;
    }

    /** Compare the four preset/inversion conventions against the transmitted bytes.
     *
     *  ★★★ THE ScF-CRC OF FRAME n IS TRANSMITTED IN FRAME n-1. That one sentence is why the
     *  first attempt matched NOTHING — 2819 frames checked and zero hits under all four
     *  conventions, which is the signature of comparing against the wrong bytes entirely rather
     *  than of a wrong preset. The CRC carried by a frame describes the scale factors of the
     *  frame that FOLLOWS it, so the bytes have to be held over.
     *  ★ Zero out of 2819 was the useful result: a wrong preset would still have matched
     *    occasionally by chance at one byte in 256. Nothing at all means structurally wrong. */
    void tallyScfCrc(const uint8_t* frame, int frameBytes, int bitrateKbps) {
        const int ncrc = bitrateKbps >= 56 ? 4 : 2;
        const int at = frameBytes - 2 - ncrc;              // before the two F-PAD bytes
        if (at < 4 || scfIdx_.empty()) { prevScfValid_ = false; return; }
        static const int loA[4] = { 0, 4, 8, 16 }, hiA[4] = { 3, 7, 15, 26 };
        const uint8_t inits[4]  = { 0x00, 0xFF, 0x00, 0xFF };
        const bool    invs[4]   = { false, false, true,  true  };

        if (prevScfValid_ && prevScfN_ == ncrc) {
            ++scfTally_.checked;
            for (int v = 0; v < 4; ++v) {
                bool all = true;
                for (int g = 0; g < ncrc; ++g)
                    if (scfCrc8(loA[g], hiA[g], inits[v], invs[v]) != prevScf_[g]) { all = false; break; }
                if (all) ++scfTally_.ok[v];
            }
        }
        // Carry THIS frame's bytes forward — they describe the NEXT frame's scale factors.
        for (int g = 0; g < ncrc; ++g) prevScf_[g] = frame[at + g];
        prevScfN_ = ncrc;
        prevScfValid_ = true;
    }
};

}  // namespace vibedab
