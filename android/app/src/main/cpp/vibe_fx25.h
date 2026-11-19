// vibe_fx25.h — V12: FX.25, the Reed-Solomon wrapper round an ordinary AX.25 frame. docs/v12/AX25-9600-FX25-IL2P.md.
//
// ★★★ FROM THE PUBLISHED DRAFT, our own code: "FX.25 Forward Error Correction Extension to AX.25 Link Protocol for
//     Amateur Packet Radio", Stensat Group, draft 0.01.06 (2006). What it says, and what this file does with it:
//       - frame = preamble (0x7E) · CORRELATION TAG (64 bits) · FEC CODEBLOCK · postamble (0x7E);
//       - the codeblock = the AX.25 packet exactly as an AX.25 TNC sends it (flags, bit-stuffing, FCS) + pad + the RS
//         check bytes; "All bytes/octets in the FX.25 frame are transmitted LSb first";
//       - the tag selects the code (Table 1): Tag_01 … Tag_0B, RS(255,239), (144,128), (80,64), (48,32), (255,223),
//         (160,128), (96,64), (64,32), (255,191), (192,128), (128,64); tag values copied from the table (they are
//         facts — the Gold codes the draft defines). "Transmission order of the bytes for Tag_01 would be 0x3E 0x2F …":
//         the 64-bit value goes low byte first, each byte LSb first — the tag is the value read LSb-first off the air;
//       - "add data scrambling, if required by transmission channel; NRZI encode the data" happens AFTER all of this,
//         so the receiver works on the NRZI-decoded (and, at 9600, descrambled) DATA bits — the HDLC deframer's bits.
//     RS parameters: field 0x11D, α = 2, first root 1 (vibe_rs8.h says how that was checked).
//
// ★★ WHY IT HELPS: the AX.25 frame is still inside, so a plain TNC decodes it when it arrives clean; when it does not,
//    the codeblock corrects up to 8/16/32 bad BYTES. A frame recovered here must still pass its own FCS-16 after the
//    correction, so FX.25 adds no false frames: a noise "tag" (≤ 5 of 64 bits wrong, ~10⁻¹¹ a bit) would also need an
//    RS decode AND a 16-bit FCS to pass.
// ★ The pad: the draft asks for 0x7E bytes after the end flag (a fraction aligned to the MSb of the last byte). Our
//   encoder (tests) keeps the flag pattern running bit for bit to the end of the data space, which is the same thing
//   to every receiver (consecutive flags are null frames) and what real transmitters do (the off-air block in
//   test-ax25hs is padded that way: fc fc fc …).
#pragma once
#include "vibe_hdlc.h"
#include "vibe_rs8.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace vibefx25 {

struct Mode { int tag; uint64_t value; int n, k; };
/** Table 1 of the draft — the tags that name a code. Tag_00, Tag_0C… and Tag_40 are reserved or undefined. */
inline const Mode* modes(int& count) {
    static const Mode m[] = {
        { 0x01, 0xB74DB7DF8A532F3EULL, 255, 239 }, { 0x02, 0x26FF60A600CC8FDEULL, 144, 128 },
        { 0x03, 0xC7DC0508F3D9B09EULL, 80, 64 },   { 0x04, 0x8F056EB4369660EEULL, 48, 32 },
        { 0x05, 0x6E260B1AC5835FAEULL, 255, 223 }, { 0x06, 0xFF94DC634F1CFF4EULL, 160, 128 },
        { 0x07, 0x1EB7B9CDBC09C00EULL, 96, 64 },   { 0x08, 0xDBF869BD2DBB1776ULL, 64, 32 },
        { 0x09, 0x3ADB0C13DEAE2836ULL, 255, 191 }, { 0x0A, 0xAB69DB6A543188D6ULL, 192, 128 },
        { 0x0B, 0x4A4ABEC4A724B796ULL, 128, 64 },
    };
    count = int(sizeof m / sizeof m[0]);
    return m;
}
/** One RS codec per check-byte count (16 / 32 / 64), built once. */
inline const vibers::Rs& codec(int nroots) {
    static const vibers::Rs r16(16, 1), r32(32, 1), r64(64, 1);
    return nroots == 16 ? r16 : nroots == 32 ? r32 : r64;
}
inline int popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(x);
#else
    int c = 0; while (x) { x &= x - 1; ++c; } return c;
#endif
}

/** ★ Extract the first AX.25 frame (FCS good) from data bits packed LSB-first — the codeblock's data part. Bytes out
 *  WITHOUT the FCS. Shared with nothing else on purpose: inside a codeblock there are no levels, only bits. */
inline bool extractFrame(const uint8_t* bytes, int nbytes, std::vector<uint8_t>& out) {
    std::vector<uint8_t> bits;
    bits.reserve(size_t(nbytes) * 8);
    uint8_t sr = 0; bool in = false; int ones = 0;
    std::vector<uint8_t> cur;
    for (int i = 0; i < nbytes * 8; ++i) {
        const int bit = (bytes[i >> 3] >> (i & 7)) & 1;
        sr = uint8_t((sr >> 1) | (bit << 7));
        bits.push_back(uint8_t(bit));
        if (bit) { if (++ones >= 7) { in = false; bits.clear(); continue; } } else ones = 0;
        if (sr == 0x7E) {
            if (in && bits.size() > 8) {
                // unstuff the content (everything before the flag's 8 bits)
                const size_t n = bits.size() - 8;
                cur.clear(); int o = 0, nb = 0; uint8_t b = 0; bool bad = false;
                for (size_t j = 0; j < n; ++j) {
                    if (o == 5) { if (bits[j]) { bad = true; break; } o = 0; continue; }
                    if (bits[j]) ++o; else o = 0;
                    b = uint8_t(b | (bits[j] << nb));
                    if (++nb == 8) { cur.push_back(b); b = 0; nb = 0; }
                }
                if (!bad && nb == 0 && cur.size() >= 17 && vibehdlc::fcsOk(cur.data(), cur.size())) {
                    out.assign(cur.begin(), cur.end() - 2);
                    return true;
                }
            }
            in = true; bits.clear();
        }
    }
    return false;
}

struct Result { std::vector<uint8_t> bytes; int fixed = 0; int tag = 0; };

/** The receiver, one per slicer: feed every DATA bit (after NRZI, after any descrambler). */
class Rx {
public:
    explicit Rx(int maxTagErrors = 5) : maxErr_(maxTagErrors) {}
    template <class Out> void bit(int b, Out&& out) {
        if (st_ == 0) {
            sr_ = (sr_ >> 1) | (uint64_t(b & 1) << 63);
            if (++have_ < 64) return;
            int nm = 0; const Mode* ms = modes(nm);
            for (int i = 0; i < nm; ++i) if (popcount64(sr_ ^ ms[i].value) <= maxErr_) {
                mode_ = &ms[i]; st_ = 1; blk_.assign(size_t(mode_->n), 0); nbits_ = 0; ++tags_;
                return;
            }
            return;
        }
        // collecting the codeblock, LSb first
        if (b & 1) blk_[size_t(nbits_ >> 3)] = uint8_t(blk_[size_t(nbits_ >> 3)] | (1 << (nbits_ & 7)));
        if (++nbits_ < mode_->n * 8) return;
        st_ = 0; have_ = 0; sr_ = 0;
        const int nroots = mode_->n - mode_->k;
        const int fixed = codec(nroots).decode(blk_.data(), mode_->n);
        if (fixed < 0) { ++rsFail_; return; }
        Result r;
        if (!extractFrame(blk_.data(), mode_->k, r.bytes)) { ++noFrame_; return; }
        r.fixed = fixed; r.tag = mode_->tag; ++ok_;
        out(r);
    }
    bool busy() const { return st_ != 0; }
    uint64_t tags() const { return tags_; }
    uint64_t decoded() const { return ok_; }
    uint64_t rsFailed() const { return rsFail_; }
private:
    int maxErr_;
    int st_ = 0, have_ = 0, nbits_ = 0;
    uint64_t sr_ = 0;
    const Mode* mode_ = nullptr;
    std::vector<uint8_t> blk_;
    uint64_t tags_ = 0, ok_ = 0, rsFail_ = 0, noFrame_ = 0;
};

/** ★ For the test generators: one AX.25 frame (bytes WITHOUT the FCS) → the FX.25 DATA bits on the air (before NRZI):
 *  `pre` flags, the tag, the codeblock, `post` flags. `nroots` 16, 32 or 64: the smallest code that holds the
 *  stuffed frame is chosen (the draft's "the specific correlation tag will be picked based on the frame size").
 *  Empty = the frame does not fit any code with that many check bytes. */
inline std::vector<int> encodeBits(const std::vector<uint8_t>& frame, int nroots, int pre = 8, int post = 2, int* tagOut = nullptr) {
    std::vector<uint8_t> f = frame; vibehdlc::appendFcs(f);
    std::vector<int> pk;
    vibehdlc::flagBits(pk); vibehdlc::stuffBits(f, pk); vibehdlc::flagBits(pk);
    int nm = 0; const Mode* ms = modes(nm); const Mode* best = nullptr;
    for (int i = 0; i < nm; ++i) if (ms[i].n - ms[i].k == nroots && ms[i].k * 8 >= int(pk.size()) && (!best || ms[i].k < best->k)) best = &ms[i];
    if (!best) return {};
    while (int(pk.size()) < best->k * 8) vibehdlc::flagBits(pk);   // the flag pattern runs on to fill the data space
    pk.resize(size_t(best->k) * 8);
    std::vector<uint8_t> blk(size_t(best->n), 0);
    for (size_t i = 0; i < pk.size(); ++i) if (pk[i]) blk[i >> 3] = uint8_t(blk[i >> 3] | (1 << (i & 7)));
    codec(nroots).encode(blk.data(), best->k, blk.data() + best->k);
    std::vector<int> out;
    for (int i = 0; i < pre; ++i) vibehdlc::flagBits(out);
    for (int i = 0; i < 64; ++i) out.push_back(int((best->value >> i) & 1));
    for (int i = 0; i < best->n * 8; ++i) out.push_back((blk[size_t(i >> 3)] >> (i & 7)) & 1);
    for (int i = 0; i < post; ++i) vibehdlc::flagBits(out);
    if (tagOut) *tagOut = best->tag;
    return out;
}

} // namespace vibefx25
