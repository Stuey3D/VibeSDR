// vibe_dab_aac.h — DAB+ audio super frames: de-interleave, Reed-Solomon, AU extraction and
// ADTS reframing for the browser's decoder.
//
// TS 102 563. The licence position depends on exactly what this file does and does not do:
// it parses, error-corrects and REFRAMES. It never produces PCM, and VibeServer links no AAC
// decoder — the browser's own does the decoding, which it is already licensed for.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "vibe_dab_rs.h"
#include "vibe_dab_superframe.h"

namespace vibedab {

struct SuperFrame {
    AudioFormat fmt{};
    std::vector<std::vector<uint8_t>> aus;   ///< access units, in order
    int  rsCorrected   = 0;                  ///< bytes fixed by Reed-Solomon this super frame
    int  rsUncorrected = 0;                  ///< codewords beyond the code's power
    bool firecodeOk    = false;
    bool valid         = false;
    /* ★★ THE CHANNEL MODE AND PS FLAG, which were parsed and thrown away. ADTS needs the CORE
     *  channel count — 1 for mono AND for HE-AAC v2, where parametric stereo reconstructs the
     *  second channel from a mono core and is signalled implicitly. Writing 2 there for a PS
     *  stream tells the decoder to expect something the bitstream does not contain. */
    bool stereo        = false;   ///< aac_channel_mode: the CORE is two channels
    bool ps            = false;   ///< parametric stereo — mono core, stereo output
};

/** CRC-16-CCITT as TS 102 563 specifies for the header firecode field: G(x) = x^16+x^12+x^5+1,
 *  register preset to all ones, complemented before transmission. */
/** ★★★ THE DAB+ HEADER FIRE CODE IS NOT CRC-16-CCITT.
 *
 *  TS 102 563 clause 5.2 gives the generator as g(x) = (x^11 + 1)(x^5 + x^3 + x^2 + x + 1), which
 *  expands to x^16 + x^14 + x^13 + x^12 + x^11 + x^5 + x^3 + x^2 + x + 1 — 0x782F, with a ZERO
 *  initial state and no final complement. It is a Fire code, chosen because it CORRECTS a burst;
 *  CCITT is a different polynomial that happens to be the right width.
 *
 *  ★★★ MEASURED ON AIR: with CCITT here, 2049 super frames were assembled from 2053 logical
 *      frames and NOT ONE passed. Every DAB+ service was silent, which is most of the UK. Nothing
 *      caught it because test-dab-aac.cpp never exercises the firecode at all — and a test that
 *      built a header with this same function would have agreed with the bug, which is a shape
 *      this project has been bitten by before.
 *  ★ The AU CRCs below are a different thing and DO use the standard DAB CRC-16. Both live in
 *    this file; only the header one was wrong. */
inline uint16_t dabFireCode16(const uint8_t* d, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x782F) : uint16_t(c << 1);
    }
    return c;
}

inline uint16_t dabCrc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}

/** Decode one transported super frame.
 *
 *  @param wire  120 x subchannel_index bytes, as five DAB logical frames deliver them
 *  @param index subchannel_index = sub-channel size in kbit/s / 8   (1..24)
 *
 *  ★★ THE VIRTUAL INTERLEAVER, per clause 6.2: "an array of dimensions subchannel_index rows by
 *     120 columns … filling byte by byte from top to bottom and from left to right". So the wire
 *     order is column-major and each ROW is one RS codeword — which is the whole point: a burst
 *     that wipes out consecutive wire bytes lands one byte in each of many codewords, where five
 *     correctable bytes per codeword can absorb it, instead of destroying one codeword outright.
 */
inline SuperFrame decodeSuperFrame(const uint8_t* wire, size_t n, int index) {
    SuperFrame sf;
    if (!wire || index < 1 || index > 24) return sf;
    const size_t need = size_t(index) * 120;
    if (n < need) return sf;

    // De-interleave into `index` rows of 120, correcting each.
    std::vector<uint8_t> rows(need);
    for (int r = 0; r < index; ++r)
        for (int c = 0; c < 120; ++c)
            rows[size_t(r) * 120 + size_t(c)] = wire[size_t(c) * size_t(index) + size_t(r)];

    for (int r = 0; r < index; ++r) {
        const int got = rsDecode120(&rows[size_t(r) * 120]);
        if (got < 0) ++sf.rsUncorrected;
        else          sf.rsCorrected += got;
    }

    // The super frame is the first 110 columns, read back in the order it was fed in.
    std::vector<uint8_t> data(size_t(index) * 110);
    for (int c = 0; c < 110; ++c)
        for (int r = 0; r < index; ++r)
            data[size_t(c) * size_t(index) + size_t(r)] = rows[size_t(r) * 120 + size_t(c)];

    if (data.size() < 8) return sf;

    // ── he_aac_super_frame_header (table 2) ─────────────────────────────────
    const uint16_t fire = uint16_t((data[0] << 8) | data[1]);
    sf.firecodeOk = (fire == dabFireCode16(&data[2], 9));

    const uint8_t b2   = data[2];
    const bool dacRate = (b2 >> 6) & 1;
    const bool sbr     = (b2 >> 5) & 1;
    const bool chMode  = (b2 >> 4) & 1;
    const bool ps      = (b2 >> 3) & 1;
    sf.stereo = chMode;
    sf.ps     = ps;
    sf.fmt = dabPlusFormat(dacRate, sbr);

    // au_start[1..n-1] are 12-bit; au_start[0] is not transmitted — "The first AU always starts
    // immediately after the he_aac_super_frame_header()".
    const int nau = sf.fmt.accessUnits;
    std::vector<size_t> start(size_t(nau) + 1, 0);
    size_t bitPos = 3 * 8;                                  // after firecode + the parameter byte
    const size_t headerBytes = 3 + size_t((nau - 1) * 12 + 7) / 8
                             + ((!(dacRate && sbr)) ? 1 : 0);   // byte alignment, per table 2
    start[0] = headerBytes;
    for (int i = 1; i < nau; ++i) {
        size_t v = 0;
        for (int b = 0; b < 12; ++b) {
            const size_t byte = bitPos >> 3;
            if (byte >= data.size()) return sf;
            v = (v << 1) | ((data[byte] >> (7 - (bitPos & 7))) & 1);
            ++bitPos;
        }
        start[size_t(i)] = v;
    }
    start[size_t(nau)] = data.size();

    for (int i = 0; i < nau; ++i) {
        const size_t a = start[size_t(i)], b = start[size_t(i) + 1];
        // ★ Every AU carries its own 16-bit CRC; a bad one is DROPPED rather than handed on. A
        //   corrupt access unit makes a decoder produce noise, which is worse than a gap.
        if (b <= a || b > data.size() || b - a < 3) continue;
        const size_t len = b - a - 2;
        const uint16_t want = uint16_t((data[b - 2] << 8) | data[b - 1]);
        if (want != uint16_t(~dabCrc16(&data[a], len))) continue;
        sf.aus.emplace_back(data.begin() + long(a), data.begin() + long(a + len));
    }
    sf.valid = !sf.aus.empty();
    return sf;
}

/** Wrap one AU in an ADTS header so a browser/OS decoder will take it.
 *
 *  ★★★ THE HEADER CARRIES THE **CORE** SAMPLE RATE, NOT THE OUTPUT RATE. Under SBR the decoder
 *      doubles it itself; writing the output rate here is the 2x chipmunk, committed by us in the
 *      one place we still touch DAB+ audio. See vibe_dab_superframe.h — this is why that file
 *      exists and why its test pins both wrong ratios by name.
 */
inline std::vector<uint8_t> toAdts(const uint8_t* au, size_t n, const AudioFormat& fmt, int channels) {
    std::vector<uint8_t> out;
    const int sfi = mpeg4SamplingFrequencyIndex(fmt.coreRateHz);
    if (sfi < 0 || n == 0) return out;
    const size_t total = n + 7;
    out.resize(total);
    const int profile = 1;                                   // AAC-LC; SBR/PS are implicit
    out[0] = 0xFF;
    out[1] = 0xF1;                                           // MPEG-4, no CRC
    out[2] = uint8_t((profile << 6) | (sfi << 2) | ((channels >> 2) & 1));
    out[3] = uint8_t(((channels & 3) << 6) | ((total >> 11) & 3));
    out[4] = uint8_t((total >> 3) & 0xFF);
    out[5] = uint8_t(((total & 7) << 5) | 0x1F);
    out[6] = 0xFC;
    std::memcpy(out.data() + 7, au, n);
    return out;
}

}  // namespace vibedab
