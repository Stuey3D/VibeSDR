// decoders/paging_decoder.cpp — see paging_decoder.h. POCSAG from ITU-R M.584-2 Annex 1.
#include "decoders/paging_decoder.h"

#include <array>
#include <cmath>
#include <string>

namespace vibe {

namespace {
constexpr uint32_t kGen = 0x769u;          // x¹⁰+x⁹+x⁸+x⁶+x⁵+x³+1 (M.584-2 §1.4)
inline int pop32(uint32_t v) { return __builtin_popcount(v); }

/** Remainder of a 31-bit block (bits 30..0) divided by the generator. */
uint32_t bchRem(uint32_t v31) {
    for (int b = 30; b >= 10; --b)
        if (v31 & (1u << b)) v31 ^= kGen << (b - 10);
    return v31 & 0x3FFu;
}
/** Syndrome → the 31-bit error pattern of every 1- and 2-bit error (t = 2: they are all distinct). */
const std::array<uint32_t, 1024>& synTable() {
    static const std::array<uint32_t, 1024> t = [] {
        std::array<uint32_t, 1024> a{};
        for (int i = 0; i < 31; ++i) {
            a[bchRem(1u << i)] = 1u << i;
            for (int j = i + 1; j < 31; ++j) a[bchRem((1u << i) | (1u << j))] = (1u << i) | (1u << j);
        }
        return a;
    }();
    return t;
}
const char kNumeric[17] = "0123456789*U -][";   // Table 3: 1010 spare (shown *), 1011 U, 1100 space, 1101 hyphen, 1110 ], 1111 [
} // namespace

uint32_t PagingDecoder::encodeWord(uint32_t info21) {
    const uint32_t v = (info21 & 0x1FFFFFu) << 10;
    const uint32_t cw31 = v | bchRem(v);
    return (cw31 << 1) | (uint32_t)(pop32(cw31) & 1);
}

int PagingDecoder::correctWord(uint32_t& cw) {
    uint32_t v31 = cw >> 1;
    const uint32_t s = bchRem(v31);
    int k = 0;
    if (s) {
        const uint32_t e = synTable()[s];
        if (!e) return -1;
        v31 ^= e; k = pop32(e);
    }
    const bool parityBad = ((pop32(v31) + (int)(cw & 1u)) & 1) != 0;
    if (k + (parityBad ? 1 : 0) > 2) return -1;
    cw = (v31 << 1) | (uint32_t)(pop32(v31) & 1);
    return k + (parityBad ? 1 : 0);
}

// ── one bit rate's receiver ──────────────────────────────────────────────────────────────────────────────────────────
struct PagingDecoder::Rx {
    PagingDecoder* o;
    int   baud;
    float inc, invInc;         // bits per sample, and its reciprocal (no divide per sample)
    float aFast, aSlow, aLp;   // one-pole coefficients
    float crossAt;             // where a zero crossing lands in the bit after the low-pass's delay (phase units)
    float dc = 0, lp = 0, lpPrev = 0, ph = 0, acc = 0;
    float dInc = 0;            // ★ the loop's second order: the transmitter's clock error, learnt (bits per sample)
    bool  lpPos = false;
    uint32_t sr = 0;
    enum class St { Hunt, Batch, ExpectSync } st = St::Hunt;
    bool  inv = false;
    bool  counted = false;     // the last batch passed the noise guard
    int   nBits = 0, nWords = 0;
    uint32_t cur = 0;
    std::array<uint32_t, 16> w{};
    // the page being assembled
    bool  inMsg = false;
    Page  pg;
    std::vector<uint32_t> chunks;

    Rx(PagingDecoder* owner, int b, double fs) : o(owner), baud(b) {
        inc = (float)(b / fs); invInc = 1.0f / inc;
        aFast = 1.0f - std::exp(-(float)(b / fs) / 6.0f);       // ≈ 6 bits (the preamble is balanced)
        aSlow = 1.0f - std::exp(-(float)(b / fs) / 150.0f);     // ≈ 150 bits once locked
        aLp   = 1.0f - std::exp(-2.0f * (float)M_PI * 0.8f * (float)(b / fs));
        crossAt = ((1.0f - aLp) / aLp) * inc;                   // the one-pole's group delay, in bits
        crossAt -= std::floor(crossAt);
    }

    void feed(const float* x, int n) {
        for (int i = 0; i < n; ++i) {
            const float a = (st == St::Hunt) ? aFast : aSlow;
            dc += a * (x[i] - dc);
            const float v = x[i] - dc;
            lpPrev = lp;
            lp += aLp * (v - lp);
            const bool pos = lp >= 0.0f;
            if (pos != lpPos) {   // a crossing: pull the bit clock so it falls at crossAt
                lpPos = pos;
                // ★ where between the two samples (one divide per CROSSING, not per sample): the phase then
                const float fr = lpPrev / (lpPrev - lp);
                // ★ ph is the bit phase at the END of the previous sample's interval (time t − ½)
                float e = ph - (1.5f - fr) * inc - crossAt;
                if (e > 0.5f) e -= 1.0f; else if (e < -0.5f) e += 1.0f;
                const bool hunt = st == St::Hunt;
                ph -= (hunt ? 0.12f : 0.04f) * e;
                // ★ a first-order loop leaves a standing phase error of drift ÷ gain (0.25 bit at 0.5 % clock error):
                //   integrate the error into the rate as well, clamped to ±2 %
                dInc -= (hunt ? 0.004f : 0.0015f) * e * inc;
                if (dInc > 0.02f * inc) dInc = 0.02f * inc; else if (dInc < -0.02f * inc) dInc = -0.02f * inc;
                if (ph < 0.0f) ph += 1.0f;
            }
            // ★ each sample stands for the interval t ± ½; ph is the phase at its END. The share past the bit edge opens the
            //   next bit, so a bit is integrated over exactly its own time at any samples-per-bit (5 at 2400 on 12 kHz).
            ph += inc + dInc;
            if (ph >= 1.0f) {
                ph -= 1.0f;
                const float after = ph * invInc;          // 0..1 of this sample's interval past the edge
                acc += v * (1.0f - after);
                bit(acc > 0.0f ? 1u : 0u);
                acc = v * after;
            } else acc += v;
        }
    }

    void bit(uint32_t b) {
        sr = (sr << 1) | b;
        switch (st) {
        case St::Hunt: {
            const int e = pop32(sr ^ kSync), ei = pop32(sr ^ ~kSync);
            if (e <= 2 || ei <= 2) { inv = ei < e; st = St::Batch; nBits = 0; nWords = 0; cur = 0; }
            break;
        }
        case St::Batch:
            cur = (cur << 1) | (b ^ (inv ? 1u : 0u));
            if (++nBits == 32) {
                w[(size_t)nWords++] = cur; nBits = 0; cur = 0;
                if (nWords == 16) { batch(); if (st == St::Batch) { st = St::ExpectSync; nBits = 0; } }
            }
            break;
        case St::ExpectSync:
            if (++nBits == 32) {
                if (pop32(sr ^ (inv ? ~kSync : kSync)) <= 4) { st = St::Batch; nBits = 0; nWords = 0; cur = 0; }
                else lose();
            }
            break;
        }
    }

    void lose() {
        if (inMsg) { pg.truncated = true; emit(); }
        st = St::Hunt; counted = false;
    }

    void batch() {
        std::array<uint32_t, 16> c = w;
        std::array<int, 16> k{};
        int good = 0;
        for (int i = 0; i < 16; ++i) { k[(size_t)i] = correctWord(c[(size_t)i]); good += (k[(size_t)i] >= 0 && k[(size_t)i] <= 1); }
        // ★★★ the noise guard (see the header): a batch counts only if ≥ 12 of 16 words are clean or one bit out
        if (good < 12) { lose(); return; }
        counted = true;
        for (int i = 0; i < 16; ++i) {
            const uint32_t cw = c[(size_t)i];
            if (k[(size_t)i] < 0) { if (inMsg) { pg.truncated = true; emit(); } continue; }
            if ((cw & 0x80000000u) == 0) {               // address (or idle): ends any message
                if (inMsg) emit();
                if (cw == kIdle) continue;
                if (k[(size_t)i] > 1) continue;          // ★ an address must be clean or one bit out (a slipped word is not)
                inMsg = true; chunks.clear();
                pg = Page{};
                pg.proto = "POCSAG"; pg.baud = baud; pg.inverted = inv;
                pg.address = (((cw >> 13) & 0x3FFFFu) << 3) | (uint32_t)(i / 2);
                pg.function = (int)((cw >> 11) & 3u);
                pg.corrected = k[(size_t)i];
            } else if (inMsg) {
                chunks.push_back((cw >> 11) & 0xFFFFFu);
                pg.corrected += k[(size_t)i];
            }
        }
    }

    static std::string decodeNumeric(const std::vector<uint32_t>& ch) {
        std::string s; uint32_t v = 0; int nb = 0;
        for (uint32_t c : ch)
            for (int b = 19; b >= 0; --b) {
                v |= ((c >> b) & 1u) << nb;
                if (++nb == 4) { s += kNumeric[v]; v = 0; nb = 0; }
            }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }
    static std::string decodeAlpha(const std::vector<uint32_t>& ch, int* printable, int* total) {
        std::string s; uint32_t v = 0; int nb = 0; *printable = 0; *total = 0;
        for (uint32_t c : ch)
            for (int b = 19; b >= 0; --b) {
                v |= ((c >> b) & 1u) << nb;
                if (++nb == 7) {
                    if (v >= 0x20 && v < 0x7F) { s += (char)v; ++*printable; ++*total; }
                    else if (v == '\n' || v == '\r') { s += ' '; ++*printable; ++*total; }
                    else if (v != 0 && v != 0x03 && v != 0x04 && v != 0x17) ++*total;   // NUL / ETX / EOT / ETB: fill
                    v = 0; nb = 0;
                }
            }
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    void emit() {
        inMsg = false;
        if (pg.truncated && chunks.empty()) return;      // ★ an address whose message never came is not a page
        if (chunks.empty()) pg.kind = "tone";
        else {
            int pr = 0, tot = 0;
            const std::string a = decodeAlpha(chunks, &pr, &tot);
            // ★ M.584 defines function 00 = numeric and 11 = alphanumeric; 01/10 carry whichever reads as text
            const bool alpha = pg.function == 3 || (pg.function != 0 && tot > 0 && pr * 10 >= tot * 9);
            pg.kind = alpha ? "alpha" : "numeric";
            pg.text = alpha ? a : decodeNumeric(chunks);
        }
        chunks.clear();
        if (o->onPage) o->onPage(pg);
    }
};

// ── FLEX ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// Our own receiver from the free published material named in paging_decoder.h (sources per fact: the doc's section 17).
//
// ★★ THE FRAME (1.875 s): Sync 1 at 1600 bit/s 2-level — a 32-bit comma (alternating), the A word (16-bit mode code +
//    0xA6C6), B (0xAAAA), A inverted — then the Frame Information Word (cycle, frame), all 1600/2; then Sync 2 (25 ms at
//    the frame's own speed, skipped by time) and 11 blocks of 160 ms. Each block carries 256 bits PER PHASE, 8 words per
//    phase interleaved bit by bit (bit k of word c is the phase's (8k + c)-th bit). 1600 bit/s: phase A. 3200: A and C.
//    6400: A, B, C, D (at 3200 baud: an even symbol's two bits → A, B; an odd one's → C, D; 3200/2: A, C alternate;
//    1600/4: a symbol's MSB → A, its LSB → C).
// ★★ 4-LEVEL: −4.8, −1.6, +1.6, +4.8 kHz carry 00, 01, 11, 10 (Gray): the MSB is the sign (as the 2-level bit), the LSB
//    is "inner". The sense of "1" is not assumed: Sync 1 is matched both ways, and the DATA bits take the opposite sense
//    to the one the sync constants matched in (measured on a public recording — the constants are written in the
//    other sense).
// ★★ A WORD: BCH(31,21) + even parity, exactly POCSAG's code (M.584-2) — but its fields go LEAST significant bit first.
// ★★★ NEVER ON NOISE: a frame is read only after a 64-bit sync match (≤ 4 bits wrong, in one of 8 patterns) — on random
//     bits about once in 10¹³ bits — and a FIW whose BCH and checksum pass; a phase only with a clean block information
//     word; a page only with its vector's checksum, every one of its words decodable, and (alphanumeric) its 10-bit check.
// ★ COST: one per-sample loop like a POCSAG rate's (DC tracker, crossing DPLL, integrate-and-dump) — per-sample feedback,
//   nothing to put in lanes; the frame parse is a few hundred word operations every 1.875 s. No per-sample divide or round.
namespace {
inline uint32_t rev21(uint32_t v) { uint32_t r = 0; for (int k = 0; k < 21; ++k) r |= ((v >> k) & 1u) << (20 - k); return r; }
constexpr uint64_t kFlexSyncTail = 0xA6C6AAAAull;   // the A word's fixed half + B
struct FlexSpeed { uint16_t code; int bps, levels, baud; };
// ★ The mode codes: 1600/2 and 3200/4 (= 6400 bit/s) confirmed independently (a hobbyist page; a public recording);
//   the other two from reading multimon-ng's source (see the header).
const FlexSpeed kFlexSpeeds[] = { {0x870C, 1600, 2, 1600}, {0xB068, 3200, 4, 1600}, {0x7B18, 3200, 2, 3200}, {0xDEA0, 6400, 4, 3200} };
} // namespace

uint32_t PagingDecoder::flexInfo(uint32_t v) { return rev21(v & 0x1FFFFFu); }
bool PagingDecoder::flexChecksumOk(uint32_t v) {
    const uint32_t s = (v & 15u) + ((v >> 4) & 15u) + ((v >> 8) & 15u) + ((v >> 12) & 15u) + ((v >> 16) & 15u) + ((v >> 20) & 1u);
    return (s & 15u) == 15u;
}
uint32_t PagingDecoder::flexWithChecksum(uint32_t v) {
    v &= ~15u;
    const uint32_t s = ((v >> 4) & 15u) + ((v >> 8) & 15u) + ((v >> 12) & 15u) + ((v >> 16) & 15u) + ((v >> 20) & 1u);
    return v | ((15u - (s & 15u)) & 15u);
}
uint32_t PagingDecoder::flexAlphaCheck(const std::vector<uint32_t>& w) {
    uint32_t t = 0;
    for (uint32_t x : w) t += (x & 0xFFu) + ((x >> 8) & 0xFFu) + ((x >> 16) & 0x1Fu);
    if (!w.empty()) t -= (w[0] & 0xFFu) + ((w[0] >> 8) & 3u);   // the header's own check field is not summed
    return ~t & 0x3FFu;
}
uint16_t PagingDecoder::flexModeCode(int bps, int levels) {
    for (const FlexSpeed& f : kFlexSpeeds) if (f.bps == bps && f.levels == levels) return f.code;
    return 0;
}

struct PagingDecoder::FlexRx {
    PagingDecoder* o;
    double fs;
    float invFs;
    // the symbol clock (as a POCSAG rate's, at 1600 baud for Sync 1, the frame's baud for its blocks)
    float inc = 0, invInc = 0, aFast = 0, aSlow = 0, aLp = 0, crossAt = 0;
    float dc = 0, lp = 0, lpPrev = 0, ph = 0, acc = 0, dInc = 0;
    float accN = 0;
    bool  lpPos = false;
    int   baud = 1600;
    // levels: the outer level from Sync 1, the inner one learnt (decision-directed)
    float outer = 0, inner = 0;
    enum class St { Hunt, SyncTail, Fiw, Sync2, Data } st = St::Hunt;
    uint64_t sr = 0;
    bool  dataInv = false;
    int   n = 0;
    uint32_t cw = 0;
    const FlexSpeed* sp = nullptr;
    int   cycle = 0, frame = 0;
    std::array<float, 80> ring{}; int ringAt = 0;   // the last 80 symbol means: at the end of Sync 1, exactly A, B, A inverted
    long  symInBlock = 0; int block = 0; long symbol = 0;
    // per phase: 88 words (11 blocks × 8), their corrections; the block being filled
    std::array<std::array<uint32_t, 88>, 4> words{};
    std::array<std::array<int8_t, 88>, 4> corr{};
    std::array<std::array<uint32_t, 8>, 4> blk{};
    std::array<int, 4> blkBit{};
    // lock / readout
    long long sampleNo = 0, lastFrameAt = -1000000000LL;
    // alphanumeric fragments waiting for their continuation, by (capcode, message number)
    struct Frag { uint32_t cap; int num; std::string text; int frames; Page pg; };
    std::vector<Frag> frags;

    FlexRx(PagingDecoder* owner, double rate) : o(owner), fs(rate), invFs((float)(1.0 / rate)) { setBaud(1600, true); }

    void setBaud(int b, bool init) {
        const float old = inc;
        baud = b;
        inc = (float)(b / fs); invInc = 1.0f / inc;
        aFast = 1.0f - std::exp(-inc / 6.0f);
        aSlow = 1.0f - std::exp(-inc / 150.0f);
        aLp   = 1.0f - std::exp(-2.0f * (float)M_PI * 0.8f * inc);
        crossAt = ((1.0f - aLp) / aLp) * inc; crossAt -= std::floor(crossAt);
        if (!init && old > 0) dInc *= inc / old;   // the learnt clock error, rescaled
    }

    void feed(const float* x, int nIn) {
        for (int i = 0; i < nIn; ++i) {
            ++sampleNo;
            // ★ the DC is learnt on Sync 1 (balanced: comma, A, B, A inverted) and then HELD for the frame: FLEX's blocks
            //   are not balanced (an idle phase is one level for 160 ms), and a tracker following them moved every level
            //   by a sixth of the deviation — enough to wreck 4-level (measured).
            //   It is then corrected ONCE from Sync 1's own balance (syncLevels), since the fast hunting tracker follows the
            //   A word's last bits.
            if (st == St::Hunt) dc += aFast * (x[i] - dc);
            const float v = x[i] - dc;
            lpPrev = lp;
            lp += aLp * (v - lp);
            const bool pos = lp >= 0.0f;
            if (pos != lpPos) {
                lpPos = pos;
                const float fr = lpPrev / (lpPrev - lp);
                float e = ph - (1.5f - fr) * inc - crossAt;
                if (e > 0.5f) e -= 1.0f; else if (e < -0.5f) e += 1.0f;
                const bool hunt = st == St::Hunt;
                ph -= (hunt ? 0.12f : 0.03f) * e;
                dInc -= (hunt ? 0.004f : 0.001f) * e * inc;
                if (dInc > 0.02f * inc) dInc = 0.02f * inc; else if (dInc < -0.02f * inc) dInc = -0.02f * inc;
                if (ph < 0.0f) ph += 1.0f;
            }
            ph += inc + dInc;
            if (ph >= 1.0f) {
                ph -= 1.0f;
                const float w = 1.0f - ph * invInc;   // this sample's share before the edge
                acc += v * w; accN += w;
                // ★ the LATE part of the symbol only (from 0.35 of it): the transmitter's and the channel's filtering
                //   carry ≈ a quarter of the last symbol into a whole-symbol average, which a 4-level inner symbol after
                //   an opposite outer one does not survive (measured: every 4-level frame lost). One divide a SYMBOL.
                symbolOut(accN > 0.0f ? acc / accN : 0.0f);
                acc = 0.0f; accN = 0.0f;
            } else if (ph >= 0.35f) { acc += v; accN += 1.0f; }
        }
    }

    void symbolOut(float m) {
        switch (st) {
        case St::Hunt: {
            sr = (sr << 1) | (m > 0.0f ? 1u : 0u);
            ring[(size_t)ringAt] = m + dc; if (++ringAt == 80) ringAt = 0;   // ★ the RAW level (the tracker's own dc added back)
            for (const FlexSpeed& f : kFlexSpeeds) {
                const uint64_t pat = ((uint64_t)f.code << 48) | (kFlexSyncTail << 16) | (uint64_t)(uint16_t)~f.code;
                const int e = __builtin_popcountll(sr ^ pat), ei = __builtin_popcountll(sr ^ ~pat);
                if (e <= 4 || ei <= 4) {
                    sp = &f; dataInv = !(ei < e);   // ★ the data take the sense opposite to the sync constants'
                    st = St::SyncTail; n = 0;
                    break;
                }
            }
            break;
        }
        case St::SyncTail:   // the inverted A word's last 16 bits
            ring[(size_t)ringAt] = m + dc; if (++ringAt == 80) ringAt = 0;   // ★ the RAW level (the tracker's own dc added back)
            if (++n == 16) { syncLevels(); st = St::Fiw; n = 0; cw = 0; }
            break;
        case St::Fiw: {
            m -= dcFix;
            cw = (cw << 1) | ((m > 0.0f) != dataInv ? 1u : 0u);
            if (++n < 32) break;
            uint32_t c = cw;
            const int k = correctWord(c);
            const uint32_t v = rev21((c >> 11) & 0x1FFFFFu);
            if (k < 0 || k > 1 || !flexChecksumOk(v)) { st = St::Hunt; break; }
            cycle = (int)((v >> 4) & 15u); frame = (int)((v >> 8) & 0x7Fu);
            lastFrameAt = sampleNo;
            inner = outer / 3.0f;
            if (sp->baud != 1600) setBaud(sp->baud, false);
            st = St::Sync2; n = 0;
            break;
        }
        case St::Sync2:      // 25 ms at the frame's own baud (the offset is applied in data())
            if (++n == baud / 40) { st = St::Data; block = 0; symInBlock = 0; symbol = 0; blkBit = {}; for (auto& b : blk) b = {}; }
            break;
        case St::Data:
            data(m);
            break;
        }
    }

    /** ★ A + B + A inverted hold exactly 40 ones and 40 zeros, so their mean IS the slicer's residual offset and their
     *  spread about it the outer level — measured once, from the sync itself, for the whole frame. */
    float dcFix = 0;
    void syncLevels() {
        float s = 0; for (float v : ring) s += v;
        const float level0 = s * (1.0f / 80.0f);   // the true centre
        float a = 0; for (float v : ring) a += std::fabs(v - level0);
        outer = a * (1.0f / 80.0f);
        dcFix = level0 - dc;                        // what the held tracker is off by (v = x − dc)
    }
    void putBit(int phase, uint32_t b) {
        const int k = blkBit[(size_t)phase]++;
        uint32_t& w = blk[(size_t)phase][(size_t)(k & 7)];
        w = (w << 1) | b;
    }
    void data(float m) {
        m -= dcFix;
        const bool msb = (m > 0.0f) != dataInv;
        if (sp->levels == 2) {
            if (sp->baud == 1600) putBit(0, msb);
            else putBit((symbol & 1) ? 2 : 0, msb);
        } else {
            // ★ the inner/outer decision: halfway between the learnt levels (outer from Sync 1, both refined here)
            const float am = std::fabs(m), thr = 0.5f * (outer + inner);
            const bool in = am < thr;
            if (in) inner += 0.02f * (am - inner); else outer += 0.02f * (am - outer);
            if (sp->baud == 1600) { putBit(0, msb); putBit(2, in); }
            else if (!(symbol & 1)) { putBit(0, msb); putBit(1, in); }
            else { putBit(2, msb); putBit(3, in); }
        }
        ++symbol;
        if (++symInBlock == (long)baud * 160 / 1000) {   // a block's 160 ms
            for (int p = 0; p < 4; ++p)
                for (int c = 0; c < 8; ++c) {
                    uint32_t w = blk[(size_t)p][(size_t)c];
                    const int k = correctWord(w);
                    words[(size_t)p][(size_t)(block * 8 + c)] = k >= 0 ? rev21((w >> 11) & 0x1FFFFFu) : 0;
                    corr[(size_t)p][(size_t)(block * 8 + c)] = (int8_t)k;
                }
            blkBit = {}; for (auto& b : blk) b = {};
            symInBlock = 0;
            if (++block == 11) endFrame();
        }
    }

    void endFrame() {
        const int nPh = sp->bps == 1600 ? 1 : sp->bps == 3200 ? 2 : 4;
        static const int kPh2[2] = {0, 2};
        for (int q = 0; q < nPh; ++q) parsePhase(nPh == 2 ? kPh2[q] : q);
        for (auto it = frags.begin(); it != frags.end();) {   // a continuation that never came
            if (++it->frames > 128) { it->pg.text = it->text; it->pg.truncated = true; if (o->onPage) o->onPage(it->pg); it = frags.erase(it); }
            else ++it;
        }
        setBaud(1600, false);
        st = St::Hunt; sr = 0;
    }

    bool ok(int p, int i, int maxCorr = 2) const { return i >= 0 && i < 88 && corr[(size_t)p][(size_t)i] >= 0 && corr[(size_t)p][(size_t)i] <= maxCorr; }

    void parsePhase(int p) {
        const auto& w = words[(size_t)p];
        if (!ok(p, 0, 1) || !flexChecksumOk(w[0])) return;          // ★ the block information word
        const int aStart = (int)((w[0] >> 8) & 3u) + 1, vStart = (int)((w[0] >> 10) & 0x3Fu);
        if (vStart <= aStart || vStart >= 88) return;
        for (int i = aStart; i < vStart;) {
            const uint32_t aw = w[(size_t)i];
            if (!ok(p, i, 1) || aw == 0 || aw == 0x1FFFFFu) { ++i; continue; }
            bool lng = false; uint32_t cap = 0; bool known = true;
            if (aw >= 0x8001u && aw <= 0x1E0000u) cap = aw - 0x8000u;
            else if (aw >= 0x1F2800u && aw <= 0x1F781Fu) cap = aw - 0x8000u;   // the special short ranges
            else if (aw >= 1u && aw <= 0x8000u && i + 1 < vStart && ok(p, i + 1, 1)) {
                lng = true;
                const uint32_t w2 = w[(size_t)i + 1];
                if (w2 >= 0x1F7FFFu && w2 <= 0x1FFFFEu) cap = aw + (2097151u - w2) * 32768u + 2068480u;
                else known = false;                                       // ★ the other long sets: not resolved (doc)
            } else { ++i; continue; }
            const int j = vStart + (i - aStart);
            if (known) vector(p, j, cap, lng);
            i += lng ? 2 : 1;
        }
    }

    void vector(int p, int j, uint32_t cap, bool lng) {
        const auto& w = words[(size_t)p];
        if (!ok(p, j) || !flexChecksumOk(w[(size_t)j])) return;
        if (lng && !ok(p, j + 1)) return;
        const uint32_t v = w[(size_t)j];
        const int type = (int)((v >> 4) & 7u);
        Page pg; pg.proto = "FLEX"; pg.baud = sp->bps; pg.levels = sp->levels; pg.phase = (char)('A' + p);
        pg.speed = std::to_string(sp->bps) + "/" + std::to_string(sp->levels);
        pg.address = cap; pg.function = type;
        auto list = [&](int b, int nWords, std::vector<uint32_t>& out) -> bool {   // the message's words, long or short
            if (lng) { out.push_back(w[(size_t)j + 1]); --nWords; }
            for (int k = 0; k < nWords; ++k) { if (!ok(p, b + k)) return false; out.push_back(w[(size_t)(b + k)]); }
            return !out.empty();
        };
        switch (type) {
        case 5: {   // alphanumeric
            const int b = (int)((v >> 7) & 0x7Fu), nW = (int)((v >> 14) & 0x7Fu);
            std::vector<uint32_t> m;
            if (nW < 2 || !list(b, nW, m) || m.size() < 2) return;
            const uint32_t h = m[0];
            if ((h & 0x3FFu) != flexAlphaCheck(m)) return;              // ★ the fragment check
            const bool cont = (h >> 10) & 1u; const int frag = (int)((h >> 11) & 3u), num = (int)((h >> 13) & 0x3Fu);
            std::string t;
            for (size_t k = 1; k < m.size(); ++k)
                for (int sh = 0; sh < 21; sh += 7) {
                    if (k == 1 && sh == 0 && frag == 3) continue;          // the first fragment's signature
                    const uint32_t c = (m[k] >> sh) & 0x7Fu;
                    if (c >= 0x20 && c < 0x7F) t += (char)c;
                    else if (c == '\n' || c == '\r') t += ' ';
                }
            pg.kind = "alpha";
            // fragments: the first (frag 3) opens, a continuation joins its own (capcode, number)
            Frag* f = nullptr;
            for (auto& x : frags) if (x.cap == cap && x.num == num) f = &x;
            if (frag != 3 && f) { f->text += t; f->frames = 0; t = f->text; }
            if (cont) {
                if (f) { f->text = t; } else frags.push_back({cap, num, t, 0, pg});
                return;
            }
            if (f) frags.erase(frags.begin() + (f - frags.data()));
            while (!t.empty() && t.back() == ' ') t.pop_back();
            pg.text = t;
            break;
        }
        case 3: case 4: case 7: {   // numeric: standard, special, numbered
            const int b = (int)((v >> 7) & 0x7Fu), nW = (int)((v >> 14) & 7u) + 1;
            std::vector<uint32_t> m;
            if (!list(b, nW, m)) return;   // ★ as alphanumeric: n counts a long address's word 1 (its 2nd vector)
            std::string t; int skip = type == 7 ? 10 : 2; uint32_t d = 0; int nb = 0;
            for (uint32_t x : m)
                for (int k = 0; k < 21; ++k) {
                    if (skip > 0) { --skip; continue; }
                    d |= ((x >> k) & 1u) << nb;
                    if (++nb == 4) { t += kNumeric[d]; d = 0; nb = 0; }
                }
            while (!t.empty() && t.back() == ' ') t.pop_back();
            pg.kind = "numeric"; pg.text = t;
            break;
        }
        case 2: {   // short message: three digits (and five more for a long address), or a tone
            const int t2 = (int)((v >> 7) & 3u);
            if (t2 != 0) { pg.kind = "tone"; break; }
            std::string t;
            for (int k = 0; k < 3; ++k) t += kNumeric[(v >> (9 + 4 * k)) & 15u];
            if (lng) for (int k = 0; k < 5; ++k) t += kNumeric[(w[(size_t)j + 1] >> (4 * k)) & 15u];
            while (!t.empty() && t.back() == ' ') t.pop_back();
            pg.kind = "numeric"; pg.text = t;
            break;
        }
        case 6: pg.kind = "binary"; break;
        case 0: pg.kind = "secure"; break;
        default: return;            // 1: an instruction (a temporary address), not a page
        }
        if (o->onPage) o->onPage(pg);
    }

    bool locked() const { return st != St::Hunt || sampleNo - lastFrameAt < (long long)(4.0 * fs); }
};

const std::vector<PagingDecoder::ModeInfo>& PagingDecoder::modes() {
    static const std::vector<ModeInfo> m = {
        // ★★★ AUTO first; then each protocol, its "ANY" first (autoModes.ts draws PROTOCOL, then RATE where there is a choice)
        { Mode::Auto,       "auto",       "AUTO", "" },
        { Mode::Pocsag,     "pocsag",     "ANY",  "POCSAG" },
        { Mode::Pocsag512,  "pocsag512",  "512",  "POCSAG" },
        { Mode::Pocsag1200, "pocsag1200", "1200", "POCSAG" },
        { Mode::Pocsag2400, "pocsag2400", "2400", "POCSAG" },
        { Mode::Flex,       "flex",       "ANY",  "FLEX" },   // ★ no rates: every frame names its own speed
    };
    return m;
}
PagingDecoder::Mode PagingDecoder::modeFromId(const std::string& id) {
    for (const auto& i : modes()) if (id == i.id) return i.mode;
    return Mode::Auto;
}
const char* PagingDecoder::idOf(Mode m) {
    for (const auto& i : modes()) if (i.mode == m) return i.id;
    return "auto";
}

PagingDecoder::PagingDecoder(double rateHz, Mode m) : fs_(rateHz), mode_(m) { build_(); }
PagingDecoder::~PagingDecoder() = default;

void PagingDecoder::build_() {
    rx_.clear(); flex_.reset();
    const int all[3] = {512, 1200, 2400};
    const bool anyPocsag = mode_ == Mode::Auto || mode_ == Mode::Pocsag;
    for (int b : all) {
        const bool want = anyPocsag || (mode_ == Mode::Pocsag512 && b == 512)
                       || (mode_ == Mode::Pocsag1200 && b == 1200) || (mode_ == Mode::Pocsag2400 && b == 2400);
        if (want && fs_ >= 4.0 * b) rx_.emplace_back(new Rx(this, b, fs_));
    }
    // ★ FLEX needs ≥ 4 samples a symbol at 3200 baud
    if ((mode_ == Mode::Auto || mode_ == Mode::Flex) && fs_ >= 12800.0) flex_.reset(new FlexRx(this, fs_));
}
void PagingDecoder::setMode(Mode m) { if (m != mode_) { mode_ = m; build_(); } }
void PagingDecoder::reset() { build_(); }
void PagingDecoder::feed(const float* x, int n) { for (auto& r : rx_) r->feed(x, n); if (flex_) flex_->feed(x, n); }

std::string PagingDecoder::detected() const {
    for (auto& r : rx_)
        if (r->counted && r->st != Rx::St::Hunt) return "POCSAG " + std::to_string(r->baud);
    if (flex_ && flex_->sp && flex_->locked())
        return "FLEX " + std::to_string(flex_->sp->bps) + "/" + std::to_string(flex_->sp->levels);
    return "";
}

} // namespace vibe
