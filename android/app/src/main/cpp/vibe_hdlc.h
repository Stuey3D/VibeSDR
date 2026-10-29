// vibe_hdlc.h — V12: the HDLC back end AIS and APRS share — NRZI, flags, bit-stuffing, the 16-bit FCS, and a
// soft-decision repair of one channel bit. docs/v12/AIS-APRS.md.
//
// ★★★ FROM THE PUBLISHED STANDARDS, written fresh. ISO/IEC 13239 (HDLC framing: the 01111110 flag, a 0 stuffed after five
//     1s, seven 1s = abort, the FCS-16 with generator x¹⁶+x¹²+x⁵+1, preset all ones, sent complemented, LSB first);
//     ITU-R M.1371-5 Annex 2 §3.2.2 (AIS uses exactly that, after NRZI); AX.25 v2.2 §3.6–3.8 (APRS: the same).
//     NRZI as both use it: a 0 is sent as a CHANGE of level, a 1 as no change — so polarity never matters.
//
// ★★ ONE CHANNEL-BIT ERROR IS TWO DATA-BIT ERRORS. NRZI decodes bit i from levels i-1 and i, so a single wrong level
//    flips two neighbouring data bits (and can make or break a stuffed 0). Repair therefore works in the LEVEL domain:
//    the frame keeps every level's soft confidence, and on a bad FCS the weakest levels are flipped one at a time and the
//    frame decoded again (NRZI → unstuff → FCS). `repairMax` levels are tried; 0 = off. Each try that passes must ALSO
//    pass the caller's `validate` (an AIS length that fits its type, AX.25 address characters), because 16 bits of FCS
//    alone would let one noise frame in 65 536 / repairMax through.
// ★ Feed one LEVEL per bit with a signed soft value (sign = the level, |v| = confidence). Nothing here knows about tones,
//   phases or rates; every demodulator above feeds the same class.
// ★★ `pairDist` = 2 is for a COHERENT MSK/GMSK detector (vibe_ais.h CohPath): it decides the carrier's phase at each bit
//    boundary on alternate axes, and the NRZI data bit is then whether symbol k equals symbol k−2 (two bits apart the
//    phase has turned by 0 or ±π). One wrong symbol is again two wrong data bits — i and i+2 — and repair flips both.
//    1 (the default, AX.25 / a discriminator) is unchanged.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace vibehdlc {

/** The HDLC FCS-16 (ISO 13239; CRC-16/X-25): reflected 0x1021 (0x8408), preset 0xFFFF, the result complemented. */
inline uint16_t fcs16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= d[i];
        for (int b = 0; b < 8; ++b) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1);
    }
    return uint16_t(~c);
}
/** `n` bytes INCLUDING the two FCS bytes (sent low byte first). */
inline bool fcsOk(const uint8_t* d, size_t n) {
    if (n < 3) return false;
    const uint16_t f = fcs16(d, n - 2);
    return d[n - 2] == uint8_t(f & 0xFF) && d[n - 1] == uint8_t(f >> 8);
}
/** Append the FCS to a frame (for the test signal generators). */
inline void appendFcs(std::vector<uint8_t>& v) { const uint16_t f = fcs16(v.data(), v.size()); v.push_back(uint8_t(f)); v.push_back(uint8_t(f >> 8)); }

/** ★ For the generators: a frame's bytes → the data bits on the air (LSB first, a 0 stuffed after five 1s). */
inline void stuffBits(const std::vector<uint8_t>& bytes, std::vector<int>& out) {
    int ones = 0;
    for (uint8_t b : bytes) for (int i = 0; i < 8; ++i) {
        const int bit = (b >> i) & 1;
        out.push_back(bit);
        if (bit) { if (++ones == 5) { out.push_back(0); ones = 0; } } else ones = 0;
    }
}
inline void flagBits(std::vector<int>& out) { for (int i = 0; i < 8; ++i) out.push_back(i == 0 || i == 7 ? 0 : 1); }
/** Data bits → NRZI levels (0 = change). `level` carries across calls. */
inline void nrzi(const std::vector<int>& bits, std::vector<int>& levels, int& level) {
    for (int b : bits) { if (!b) level ^= 1; levels.push_back(level); }
}

struct Frame {
    std::vector<uint8_t> bytes;   ///< WITHOUT the FCS
    int repaired = 0;             ///< 0 = clean; 1 = one channel bit was flipped to pass the FCS
    float quality = 0;            ///< the mean |soft| over the frame (0…~1): how clean the eye was
    float flipConf = 0;           ///< a repaired frame: the |soft| of the level flipped, relative to the frame's mean
};

struct DeframerOptions {
    size_t minBytes = 3, maxBytes = 400;   ///< frame length WITH the FCS
    int repairMax = 0;                     ///< weakest levels tried on a bad FCS (0 = no repair)
    /** Extra check on a frame (bytes without the FCS). Always applied to a repaired frame; to a clean one only when
     *  `validateClean`. Empty = accept. */
    std::function<bool(const std::vector<uint8_t>&)> validate;
    bool validateClean = false;
    int pairDist = 1;                      ///< data bit = (level k == level k−pairDist): 1 = NRZI levels, 2 = coherent MSK
};

class Deframer {
public:
    using Options = DeframerOptions;
    explicit Deframer(Options o = {}) : o_(std::move(o)) {}

    /** One level: `soft` > 0 = high, < 0 = low; |soft| = confidence. Calls `out` for each frame that passes. */
    template <class Out> void push(float soft, Out&& out) {
        const int lvl = soft >= 0 ? 1 : 0;
        const int ref = o_.pairDist == 2 ? lastLvl2_ : lastLvl_;
        const int bit = lvl == ref ? 1 : 0;        // NRZI: no change = 1
        lastLvl2_ = lastLvl_;
        lastLvl_ = lvl;
        const float conf = std::fabs(soft);
        lv_.push_back(conf);
        bits_.push_back(uint8_t(bit));
        sr_ = uint8_t((sr_ >> 1) | (bit << 7));    // the last 8 data bits, oldest in bit 0
        if (bit) { if (++ones_ >= 7) { abort_(); return; } } else ones_ = 0;
        if (sr_ == 0x7E) {                          // 0 1 1 1 1 1 1 0 just completed (the flag reads 0x7E either way round)
            if (inFrame_ && bits_.size() > 8) closeFrame_(out);
            inFrame_ = true;
            bits_.clear(); lv_.clear();
            // ★ keep the closing flag's last level as the next frame's NRZI reference — it already is (lastLvl_)
            return;
        }
        if (bits_.size() > o_.maxBytes * 8 * 6 / 5 + 16) { inFrame_ = false; bits_.clear(); lv_.clear(); }
    }
    /** Inside a frame (seen a flag, no abort since) — a PLL may hold its phase more firmly. */
    bool inFrame() const { return inFrame_; }
    uint64_t framesOk() const { return ok_; }
    uint64_t framesRepaired() const { return rep_; }
    uint64_t fcsFails() const { return bad_; }

private:
    void abort_() { inFrame_ = false; bits_.clear(); lv_.clear(); ones_ = 0; }

    /** Data bits (the content between the flags) → bytes, unstuffed. False = a stuffing violation or not whole bytes. */
    static bool unstuff_(const uint8_t* b, size_t n, std::vector<uint8_t>& out) {
        out.clear();
        int ones = 0, nb = 0; uint8_t cur = 0;
        for (size_t i = 0; i < n; ++i) {
            const int bit = b[i];
            if (ones == 5) {                // this bit must be a stuffed 0
                if (bit) return false;
                ones = 0; continue;
            }
            if (bit) ++ones; else ones = 0;
            cur = uint8_t(cur | (bit << nb));
            if (++nb == 8) { out.push_back(cur); cur = 0; nb = 0; }
        }
        return nb == 0;
    }

    template <class Out> void closeFrame_(Out&& out) {
        // content = everything before the 8 flag bits just matched
        const size_t n = bits_.size() - 8;
        if (n < o_.minBytes * 8 || n > o_.maxBytes * 8 * 6 / 5 + 8) return;
        float q = 0; for (size_t i = 0; i < n; ++i) q += lv_[i];
        q /= float(n);
        Frame f;
        if (unstuff_(bits_.data(), n, tmp_) && tmp_.size() >= o_.minBytes && tmp_.size() <= o_.maxBytes && fcsOk(tmp_.data(), tmp_.size())) {
            f.bytes.assign(tmp_.begin(), tmp_.end() - 2);
            if (o_.validateClean && o_.validate && !o_.validate(f.bytes)) return;
            f.quality = q; ++ok_;
            out(f);
            return;
        }
        ++bad_;
        if (o_.repairMax <= 0) return;
        // ★ THE WEAKEST LEVELS, one at a time. A level i maps to data bits i and i+1 (both inside the content; the first
        //   level's partner is the opening flag's last bit, which we cannot change, so i starts at 1).
        idx_.clear();
        const size_t D = size_t(o_.pairDist == 2 ? 2 : 1);
        for (size_t i = 1; i + D < n; ++i) idx_.push_back(uint32_t(i));
        const size_t k = std::min<size_t>(size_t(o_.repairMax), idx_.size());
        std::partial_sort(idx_.begin(), idx_.begin() + long(k), idx_.end(), [this](uint32_t a, uint32_t b) { return lv_[a] < lv_[b]; });
        for (size_t t = 0; t < k; ++t) {
            const size_t i = idx_[t];
            bits_[i] ^= 1; bits_[i + D] ^= 1;
            const bool good = unstuff_(bits_.data(), n, tmp_) && tmp_.size() >= o_.minBytes && tmp_.size() <= o_.maxBytes && fcsOk(tmp_.data(), tmp_.size());
            bits_[i] ^= 1; bits_[i + D] ^= 1;
            if (!good) continue;
            f.bytes.assign(tmp_.begin(), tmp_.end() - 2);
            if (o_.validate && !o_.validate(f.bytes)) continue;
            f.repaired = 1; f.quality = q; f.flipConf = q > 0 ? lv_[i] / q : 1; ++rep_;
            out(f);
            return;
        }
    }

    Options o_;
    std::vector<uint8_t> bits_, tmp_;
    std::vector<float> lv_;
    std::vector<uint32_t> idx_;
    uint8_t sr_ = 0;
    int lastLvl_ = 0, lastLvl2_ = 0, ones_ = 0;
    bool inFrame_ = false;
    uint64_t ok_ = 0, rep_ = 0, bad_ = 0;
};

/** ★ A digital PLL bit clock (the textbook zero-crossing type): a phase in bits, advanced by baud/fs a sample; a level
 *  is taken (linearly interpolated) where the phase wraps; a crossing of the demodulated signal pulls the phase towards
 *  the half-bit point between two samples. `hold` (0…1) is how firmly it keeps its phase: low while hunting so the
 *  preamble pulls it in fast, higher in a frame so noise does not shake it. Feed the demodulated signal; get levels. */
class BitClock {
public:
    BitClock(double fs, double baud, double offset = 0) : step_(baud / fs), ph_(offset) {}
    void setOffset(double o) { ph_ = o; }
    /** One sample of the demodulated, DC-free signal; calls `level(soft)` when a bit is due. */
    template <class Lvl> void push(float x, float hold, Lvl&& level) {
        const double prevPh = ph_;
        ph_ += step_;
        if (ph_ >= 1.0) {
            ph_ -= 1.0;
            // the bit centre fell between the last sample and this one: interpolate there
            const double back = ph_ / step_;              // samples since the wrap point (0…1)
            const float v = float(x - (x - last_) * back);
            level(v);
        }
        if ((x >= 0) != (last_ >= 0) && x != last_) {
            // the crossing's phase: between prevPh and ph_ (unwrapped), at the zero of the line last_ → x
            const double frac = double(last_) / double(last_ - x);
            double pc = prevPh + step_ * frac;
            if (pc >= 1.0) pc -= 1.0;
            double e = pc - 0.5;                          // a crossing belongs half-way between bit centres
            ph_ -= e * (1.0 - double(hold));
            if (ph_ < 0) ph_ += 1.0;
            if (ph_ >= 1.0) ph_ -= 1.0;
        }
        last_ = x;
    }
private:
    double step_, ph_;
    float last_ = 0;
};

} // namespace vibehdlc
